from __future__ import print_function

import os
from os.path import  dirname, join, abspath, basename
import sys
import re
import pipes
import math
import random
import shlex
import struct
import subprocess32

from sqlalchemy import Table, Column, ForeignKey, create_engine
from sqlalchemy.types import Integer, Text, Float, BigInteger, Boolean
from sqlalchemy.ext.declarative import declarative_base
from sqlalchemy.dialects import postgresql
from sqlalchemy.orm import relationship, sessionmaker
from sqlalchemy.sql.expression import func

from subprocess32 import PIPE, check_call
from multiprocessing import cpu_count
from multiprocessing.pool import ThreadPool

from composite import Composite

Base = declarative_base()

debugging = False

class Loc(Composite):
    column = Integer
    line = Integer

class ASTLoc(Composite):
    filename = Text
    begin = Loc
    end = Loc

class Range(Composite):
    low = Integer
    high = Integer

class SourceLval(Base):
    __tablename__ = 'sourcelval'

    id = Column(Integer, primary_key=True)
    loc = ASTLoc.composite('loc')
    ast_name = Column(Text)

    def __str__(self):
        return 'Lval[{}](loc={}:{}, ast="{}")'.format(
            self.id, self.loc.filename, self.loc.begin.line, self.ast_name
        )

class LabelSet(Base):
    __tablename__ = 'labelset'

    id = Column(BigInteger, primary_key=True)
    ptr = Column(BigInteger)
    inputfile = Column(Text)
    labels = Column(postgresql.ARRAY(Integer))

    def __repr__(self):
        return str(self.labels)

dua_viable_bytes = \
    Table('dua_viable_bytes', Base.metadata,
          Column('object_id', BigInteger, ForeignKey('dua.id')),
          Column('index', BigInteger),
          Column('value', BigInteger, ForeignKey('labelset.id')))

class Dua(Base):
    __tablename__ = 'dua'

    id = Column(BigInteger, primary_key=True)
    lval_id = Column('lval', BigInteger, ForeignKey('sourcelval.id'))
    all_labels = Column(postgresql.ARRAY(Integer))
    inputfile = Column(Text)
    max_tcn = Column(Integer)
    max_cardinality = Column(Integer)
    instr = Column(BigInteger)
    fake_dua = Column(Boolean)

    lval = relationship("SourceLval")
    viable_bytes = relationship("LabelSet", secondary=dua_viable_bytes)

    def __str__(self):
        return 'DUA[{}](lval={}, labels={}, viable={}, input={}, instr={}, fake_dua={})'.format(
            self.id, self.lval, self.all_labels, self.viable_bytes, self.inputfile,
            self.instr, self.fake_dua
            )

class DuaBytes(Base):
    __tablename__ = 'duabytes'

    id = Column(BigInteger, primary_key=True)
    dua_id = Column('dua', BigInteger, ForeignKey('dua.id'))
    selected = Range.composite('selected')
    all_labels = Column(postgresql.ARRAY(Integer))

    dua = relationship("Dua")
    def __str__(self):
        return 'DUABytes[DUA[{}:{}, {}, {}]][{}:{}](labels={})'.format(
            self.dua.lval.loc.filename, self.dua.lval.loc.begin.line,
            self.dua.lval.ast_name, 'fake' if self.dua.fake_dua else 'real',
            self.selected.low, self.selected.high, self.all_labels)

class AttackPoint(Base):
    __tablename__ = 'attackpoint'

    id = Column(BigInteger, primary_key=True)
    loc = ASTLoc.composite('loc')
    typ = Column('type', Integer)

    # enum Type {
    FUNCTION_CALL = 0
    POINTER_READ = 1
    POINTER_WRITE = 2
    QUERY_POINT = 3
    # } type;

    def __str__(self):
        type_strs = [
            "ATP_FUNCTION_CALL",
            "ATP_POINTER_READ",
            "ATP_POINTER_WRITE",
            "ATP_QUERY_POINT",
            "ATP_PRINTF_LEAK"
        ]
        return 'ATP[{}](loc={}:{}, type={})'.format(
            self.id, self.loc.filename, self.loc.begin.line, type_strs[self.typ]
        )

build_bugs = \
    Table('build_bugs', Base.metadata,
          Column('object_id', BigInteger, ForeignKey('build.id')),
          Column('index', BigInteger, default=0),
          Column('value', BigInteger, ForeignKey('bug.id')))

class Bug(Base):
    __tablename__ = 'bug'

    # enum Type {
    PTR_ADD = 0
    RET_BUFFER = 1
    REL_WRITE = 2
    PRINTF_LEAK = 3
    # };
    type_strings = ['BUG_PTR_ADD', 'BUG_RET_BUFFER', 'BUG_REL_WRITE', 'BUG_PRINTF_LEAK']

    id = Column(BigInteger, primary_key=True)
    type = Column(Integer)
    trigger_id = Column('trigger', BigInteger, ForeignKey('duabytes.id'))
    trigger_lval_id = Column('trigger_lval', BigInteger, ForeignKey('sourcelval.id'))
    atp_id = Column('atp', BigInteger, ForeignKey('attackpoint.id'))

    trigger = relationship("DuaBytes")
    trigger_lval = relationship("SourceLval")

    max_liveness = Column(Float)

    atp = relationship("AttackPoint")

    extra_duas = Column(postgresql.ARRAY(BigInteger))

    builds = relationship("Build", secondary=build_bugs,
                          back_populates="bugs")

    def __str__(self):
        return 'Bug[{}](type={}, trigger={}, atp={})'.format(
            self.id, Bug.type_strings[self.type], self.trigger, self.atp)

class Build(Base):
    __tablename__ = 'build'

    id = Column(BigInteger, primary_key=True)
    compile = Column(Boolean)
    output = Column(Text)

    bugs = relationship("Bug", secondary=build_bugs,
                        back_populates="builds")

class Run(Base):
    __tablename__ = 'run'

    id = Column(BigInteger, primary_key=True)
    build_id = Column('build', BigInteger, ForeignKey('build.id'))
    fuzzed_id = Column('fuzzed', BigInteger, ForeignKey('bug.id'))
    exitcode = Column(Integer)
    output = Column(Text)
    success = Column(Boolean)

    build = relationship("Build")
    fuzzed = relationship("Bug")

class LavaDatabase(object):
    def __init__(self, project):
        self.project = project
        self.engine = create_engine(
            "postgresql+psycopg2://{}@/{}".format(
                "postgres", project['db']
            )
        )
        self.Session = sessionmaker(bind=self.engine)
        self.session = self.Session()

    def uninjected(self):
        return self.session.query(Bug).filter(~Bug.builds.any()).join(Bug.atp)

    # returns uninjected (not yet in the build table) possibly fake bugs
    def uninjected2(self, fake):
        return self.uninjected()\
            .join(Bug.trigger)\
            .join(DuaBytes.dua)\
            .filter(Dua.fake_dua == fake)

    def uninjected_random(self, fake):
        return self.uninjected2(fake).order_by(func.random())

    def next_bug_random(self, fake):
        count = self.uninjected2(fake).count()
        return self.uninjected2(fake)[random.randrange(0, count)]


def run_cmd(cmd, timeout=10, check=True, **kwargs):
    if type(cmd) in [str, unicode]:
        cmd = shlex.split(cmd)
    if debugging:
        print("run_cmd(" + subprocess32.list2cmdline(cmd) + ")")
    p = subprocess32.Popen(cmd, stdout=PIPE, stderr=PIPE, **kwargs)
    try:
        # returns tuple (stdout, stderr)
        output = p.communicate(timeout=timeout)
    except subprocess32.TimeoutExpired:
        print("Killing process due to timeout expiration.")
        p.terminate()
        return (-9, "timeout expired")
    if check and p.returncode != 0:
        print(output[0])
        print(output[1])
        raise RuntimeError("Subprocess failed!")
    return (p.returncode, output)

def run_cmd_notimeout(cmd, **kwargs):
    return run_cmd(cmd, timeout=None, **kwargs)

lava = 0x6c617661

# fuzz_labels_list is a list of listof tainted byte offsets within file filename.
# replace those bytes with random in a new file named new_filename
def mutfile(filename, fuzz_labels_list, new_filename, bug_id, kt=False, knob=0):
    if kt:
        assert (knob < 2**16-1)
        lava_lower = lava & 0xffff
        bug_trigger = ((lava_lower - bug_id) & 0xffff)
        magic_val = struct.pack("<I", (knob << 16) | bug_trigger)
    else:
        magic_val = struct.pack("<I", lava - bug_id)
    # collect set of tainted offsets in file.
    file_bytes = bytearray(open(filename).read())
    # change first 4 bytes in dua to magic value
    for fuzz_labels in fuzz_labels_list:
        for (i, offset) in zip(range(4), fuzz_labels):
            #print("i=%d offset=%d len(file_bytes)=%d" % (i,offset,len(file_bytes)))
            file_bytes[offset] = magic_val[i]
    with open(new_filename, 'w') as fuzzed_f:
        fuzzed_f.write(file_bytes)

# run lavatool on this file to inject any parts of this list of bugs
def run_lavatool(bug_list, lp, project_file, project, args, llvm_src, filename):
    print("Running lavaTool on [{}]...".format(filename))
    bug_list_str = ','.join([str(bug.id) for bug in bug_list])
    main_files = ','.join([join(lp.bugs_build, f) for f in project['main_file']])
    cmd = [
        lp.lava_tool, '-action=inject', '-bug-list=' + bug_list_str,
        '-src-prefix=' + lp.bugs_build, '-project-file=' + project_file,
        '-main-files=' + main_files, join(lp.bugs_build, filename)
    ]
    if args.arg_dataflow: cmd.append('-arg_dataflow')
    if args.knobTrigger != -1: cmd.append('-kt')
    return run_cmd_notimeout(cmd)

class LavaPaths(object):

    def __init__(self, project):
        self.top_dir = join(project['directory'], project['name'])
        self.lavadb = join(self.top_dir, 'lavadb')
        self.lava_dir = dirname(dirname(abspath(sys.argv[0])))
        self.lava_tool = join(self.lava_dir, 'src_clang', 'build', 'lavaTool')
        if 'source_root' in project:
            self.source_root = project['source_root']
        else:
            tar_files = subprocess32.check_output(['tar', 'tf', project['tarfile']], stderr=sys.stderr)
            self.source_root = tar_files.splitlines()[0].split(os.path.sep)[0]
        self.queries_build = join(self.top_dir, self.source_root)
        self.bugs_top_dir = join(self.top_dir, 'bugs')

    def __str__(self):
        rets = ""
        rets += "top_dir =       %s\n" % self.top_dir
        rets += "lavadb =        %s\n" % self.lavadb
        rets += "lava_dir =      %s\n" % self.lava_dir
        rets += "lava_tool =     %s\n" % self.lava_tool
        rets += "source_root =   %s\n" % self.source_root
        rets += "queries_build = %s\n" % self.queries_build
        rets += "bugs_top_dir =  %s\n" % self.bugs_top_dir
        rets += "bugs_parent =   %s\n" % self.bugs_parent
        rets += "bugs_build =    %s\n" % self.bugs_build
        rets += "bugs_install =  %s\n" % self.bugs_install
        return rets

    def set_bugs_parent(self, bugs_parent):
        assert self.bugs_top_dir == dirname(bugs_parent)
        self.bugs_parent = bugs_parent
        self.bugs_build = join(self.bugs_parent, self.source_root)
        self.bugs_install = join(self.bugs_build, 'lava-install')

# inject this set of bugs into the source place the resulting bugged-up
# version of the program in bug_dir
def inject_bugs(bug_list, db, lp, project_file, project, args, update_db):
    if not os.path.exists(lp.bugs_parent):
        os.makedirs(lp.bugs_parent)

    print("source_root = " + lp.source_root + "\n")

    # Make sure directories and btrace is ready for bug injection.
    def run(args, **kwargs):
        print("run(", subprocess32.list2cmdline(args), ")")
        check_call(args, cwd=lp.bugs_build, **kwargs)
    if not os.path.isdir(lp.bugs_build):
        print("Untarring...")
        check_call(['tar', '--no-same-owner', '-xf', project['tarfile']],
                   cwd=lp.bugs_parent)
    if not os.path.exists(join(lp.bugs_build, '.git')):
        print("Initializing git repo...")
        run(['git', 'init'])
        run(['git', 'config', 'user.name', 'LAVA'])
        run(['git', 'config', 'user.email', 'nobody@nowhere'])
        run(['git', 'add', '-A', '.'])
        run(['git', 'commit', '-m', 'Unmodified source.'])
    if not os.path.exists(join(lp.bugs_build, 'btrace.log')):
        print("Making with btrace...")
        run(shlex.split(project['configure']) + ['--prefix=' + lp.bugs_install])
        run([join(lp.lava_dir, 'btrace', 'sw-btrace')] + shlex.split(project['make']))
    sys.stdout.flush()
    sys.stderr.flush()

    llvm_src = None
    # find llvm_src dir so we can figure out where clang #includes are for btrace
    config_mak = join(lp.lava_dir, "src_clang/config.mak")
    print("config.mak = [%s]" % config_mak)
    for line in open(config_mak):
        foo = re.search("LLVM_SRC_PATH := (.*)$", line)
        if foo:
            llvm_src = foo.groups()[0]
            break
    assert llvm_src is not None
    print("llvm_src =" + llvm_src)

    if not os.path.exists(join(lp.bugs_build, 'compile_commands.json')):
        run([join(lp.lava_dir, 'btrace', 'sw-btrace-to-compiledb'),
            llvm_src + "/Release/lib/clang/3.6.2/include"])
        # also insert instr for main() fn in all files that need it
        run(['git', 'add', 'compile_commands.json'])
        run(['git', 'commit', '-m', 'Add compile_commands.json.'])
        run(shlex.split(project['make']))
        try:
            run(['find', '.', '-name', '*.[ch]', '-exec', 'git', 'add', '{}', ';'])
            run(['git', 'commit', '-m', 'Adding source files'])
        except subprocess32.CalledProcessError:
            pass
        if not os.path.exists(lp.bugs_install):
            check_call(project['install'], cwd=lp.bugs_build, shell=True)

        # ugh binutils readelf.c will not be lavaTool-able without
        # bfd.h which gets created by make.
        run(shlex.split(project["make"]))
        run(['find', '.', '-name', '*.[ch]', '-exec', 'git', 'add', '{}', ';'])
        try:
            run(['git', 'commit', '-m', 'Adding any make-generated source files'])
        except subprocess32.CalledProcessError:
            pass

    bugs_to_inject = db.session.query(Bug).filter(Bug.id.in_(bug_list)).all()

    # collect set of src files into which we must inject code
    src_files = set()
    input_files = set()

    for bug_index, bug in enumerate(bugs_to_inject):
        print("------------\n")
        print("SELECTED ")
        if bug.trigger.dua.fake_dua:
            print("NON-BUG")
        else:
            print("BUG")
        print(" {} : {}".format(bug_index, bug.id))
        print("   (%d,%d)" % (bug.trigger.dua_id, bug.atp_id))
        print("DUA:")
        print("   ", bug.trigger.dua)
        print("ATP:")
        print("   ", bug.atp)
        print("max_tcn={}  max_liveness={}".format(
            bug.trigger.dua.max_tcn, bug.max_liveness))
        src_files.add(bug.trigger_lval.loc_filename)
        src_files.add(bug.atp.loc_filename)
        input_files.add(bug.trigger.dua.inputfile)
    sys.stdout.flush()

    # cleanup
    print("------------\n")
    print("CLEAN UP SRC")
    run_cmd_notimeout("git checkout -f", cwd=lp.bugs_build)

    print("------------\n")
    print("INJECTING BUGS INTO SOURCE")
    print("%d source files: " % (len(src_files)))
    print(src_files)

    all_files = src_files | set(project['main_file'])
    pool = ThreadPool(cpu_count())
    def modify_source(filename):
        run_lavatool(bugs_to_inject, lp, project_file, project, args,
                     llvm_src, filename)
    pool.map(modify_source, all_files)

    clang_apply = join(lp.lava_dir, 'src_clang', 'build', 'clang-apply-replacements')
    def apply_replacements(src_dir):
        run_cmd_notimeout([clang_apply, '.', '-remove-change-desc-files'],
                          cwd=join(lp.bugs_build, src_dir))
    pool.map(apply_replacements, set([dirname(f) for f in all_files]))

    # paranoid clean -- some build systems need this
    if 'makeclean' in project and project['makeclean']:
        if type(project['makeclean']) == bool:
            print(lp.bugs_build)
            run_cmd_notimeout("make clean", cwd=lp.bugs_build)
        else:
            check_call(project['makeclean'], cwd=lp.bugs_build, shell=True)

    # compile
    print("------------\n")
    print("ATTEMPTING BUILD OF INJECTED BUG(S)")
    print("build_dir = " + lp.bugs_build)
    (rv, outp) = run_cmd_notimeout(project['make'], cwd=lp.bugs_build,
                                   check=False)
    build = Build(compile=(rv == 0), output=(outp[0] + ";" + outp[1]),
                  bugs=bugs_to_inject)
    if rv == 0:
        # build success
        print("build succeeded")
        check_call(project['install'], cwd=lp.bugs_build, shell=True)

    # add a row to the build table in the db
    if update_db:
        db.session.add(build)

    return (build, input_files)

def get_suffix(fn):
    split = basename(fn).split(".")
    if len(split) == 1:
        return ""
    else:
        return "." + split[-1]

# run the bugged-up program
def run_modified_program(project, install_dir, input_file, timeout):
    cmd = project['command'].format(install_dir=install_dir,input_file=input_file)
    cmd = "setarch {} -R {}".format(subprocess32.check_output("arch").strip(), cmd)
    cmd = '/bin/bash -c '+ pipes.quote(cmd)
    print(cmd)
    envv = {}
    lib_path = project.get('library_path', '{install_dir}/lib')
    lib_path = lib_path.format(install_dir=install_dir)
    envv["LD_LIBRARY_PATH"] = join(install_dir, lib_path)
    return run_cmd(cmd, cwd=install_dir, env=envv, timeout=timeout, check=False)

# find actual line number of attack point for this bug in source
def get_trigger_line(lp, bug):
    with open(join(lp.bugs_build, bug.atp.loc_filename), "r") as f:
        lava_get = "lava_get({})".format(bug.trigger.id)
        atp_lines = [line_num + 1 for line_num, line in enumerate(f) if
                    lava_get in line]
        # return closest to original begin line.
        distances = [
            (abs(line - bug.atp.loc_begin_line), line) for line in atp_lines
        ]
        return min(distances)[1]

# use gdb to get a stacktrace for this bug
def check_stacktrace_bug(lp, project, bug, fuzzed_input):
    gdb_py_script = join(lp.lava_dir, "scripts/stacktrace_gdb.py")
    lib_path = project.get('library_path', '{install_dir}/lib')
    lib_path = lib_path.format(install_dir=lp.bugs_install)
    envv = {"LD_LIBRARY_PATH": lib_path}
    cmd = project['command'].format(install_dir=lp.bugs_install,input_file=fuzzed_input)
    gdb_cmd = "gdb --batch --silent -x {} --args {}".format(gdb_py_script, cmd)
    (rc, (out, err)) = run_cmd(gdb_cmd, cwd=lp.bugs_install, env=envv) # shell=True)
    if debugging:
        for line in out.splitlines(): print(line)
        for line in err.splitlines(): print(line)
    prediction = " at {}:{}".format(basename(bug.atp.loc_filename),
                             get_trigger_line(lp, bug))
    print("Prediction {}".format(prediction))
    for line in out.splitlines():
        if bug.type == Bug.RET_BUFFER:
            # These should go into garbage code if they trigger.
            if line.startswith("#0") and line.endswith(" in ?? ()"):
                return True
        elif bug.type == Bug.PRINTF_LEAK:
            # FIXME: Validate this!
            return True
        else: # PTR_ADD or REL_WRITE for now.
            if line.startswith("#0") or \
                    bug.atp.typ == AttackPoint.FUNCTION_CALL:
                # Function call bugs are valid if they happen anywhere in
                # call stack.
                if line.endswith(prediction): return True

    return False


def unfuzzed_input_for_bug(lp, bug):
    return join(lp.top_dir, 'inputs', basename(bug.trigger.dua.inputfile))


def fuzzed_input_for_bug(lp, bug):
    unfuzzed_input = unfuzzed_input_for_bug(lp, bug)
    suff = get_suffix(unfuzzed_input)
    pref = unfuzzed_input[:-len(suff)] if suff != "" else unfuzzed_input
    return "{}-fuzzed-{}{}".format(pref, bug.id, suff)

def validate_bug(db, lp, project, bug, bug_index, build, args, update_db,
                 unfuzzed_outputs):
    unfuzzed_input = unfuzzed_input_for_bug(lp, bug)
    fuzzed_input = fuzzed_input_for_bug(lp, bug)
    print(str(bug))
    print("fuzzed = [%s]" % fuzzed_input)
    mutfile_kwargs = {}
    if args.knobTrigger != -1:
        print("Knob size: {}".format(args.knobTrigger))
        mutfile_kwargs = { 'kt': True, 'knob': args.knobTrigger }

    fuzz_labels_list = [bug.trigger.all_labels]
    if len(bug.extra_duas) > 0:
        extra_query = db.session.query(DuaBytes)\
            .filter(DuaBytes.id.in_(bug.extra_duas))
        fuzz_labels_list.extend([d.all_labels for d in extra_query])
    mutfile(unfuzzed_input, fuzz_labels_list, fuzzed_input, bug.id,
            **mutfile_kwargs)
    timeout = project.get('timeout', 5)
    (rv, outp) = run_modified_program(project, lp.bugs_install, fuzzed_input,
                                      timeout)
    print("retval = %d" % rv)
    if update_db:
        db.session.add(Run(build=build, fuzzed=bug, exitcode=rv,
                           output=(outp[0] + '\n' + outp[1]).decode('ascii', 'ignore'), success=True))
    if bug.trigger.dua.fake_dua == False:
        if bug.type == Bug.PRINTF_LEAK:
            if outp != unfuzzed_outputs[bug.trigger.dua.inputfile]:
                return True
        else:
            # this really is supposed to be a bug
            # we should see a seg fault or something
            # NB: Wrapping programs in bash transforms rv -> 128 - rv
            # so e.g. -11 goes to 139.
            if rv in [-6, -11, 134, 139]:
                print("RV indicates memory corruption")
                if args.checkStacktrace:
                    if check_stacktrace_bug(lp, project, bug, fuzzed_input):
                        print("... and stacktrace agrees with trigger line")
                        return True
                    else:
                        print("... but stacktrace disagrees with trigger line")
                        return False
                else:
                    # not checking that bug manifests at same line as trigger point
                    return True
            else:
                print("RV does not indicate memory corruption")
                return False
    else:
        # this really is supposed to be a non-bug
        # we should see a 0
        assert rv == 0
        return False

# validate this set of bugs
def validate_bugs(bug_list, db, lp, project, input_files, build, args, update_db):

    timeout = project.get('timeout', 5)

    print("------------\n")
    # first, try the original files
    print("TESTING -- ORIG INPUT")
    unfuzzed_outputs = {}
    for input_file in input_files:
        unfuzzed_input = join(lp.top_dir, 'inputs', basename(input_file))
        (rv, outp) = run_modified_program(project, lp.bugs_install,
                                          unfuzzed_input, timeout)
        unfuzzed_outputs[basename(input_file)] = outp
        if rv != 0:
            print("***** buggy program fails on original input!")
            assert False
        else:
            print("buggy program succeeds on original input", input_file)
        print("retval = %d" % rv)
        print("output:")
        lines = outp[0] + " ; " + outp[1]
        if update_db:
            db.session.add(Run(build=build, fuzzed=None, exitcode=rv,
                            output=lines, success=True))
    print("ORIG INPUT STILL WORKS\n")

    # second, try each of the fuzzed inputs and validate
    print("TESTING -- FUZZED INPUTS")
    real_bugs = []
    bugs_to_inject = db.session.query(Bug).filter(Bug.id.in_(bug_list)).all()
    for bug_index, bug in enumerate(bugs_to_inject):
        print("=" * 60)
        print("Validating bug {} of {} ". format(
            bug_index + 1, len(bugs_to_inject)))
        validated = validate_bug(db, lp, project, bug, bug_index, build,
                                 args, update_db, unfuzzed_outputs)
        if validated:
            real_bugs.append(bug.id)
        print()
    f = float(len(real_bugs)) / len(bugs_to_inject)
    print(u"yield {:.2f} ({} out of {}) real bugs (95% CI +/- {:.2f}) ".format(
        f, len(real_bugs), len(bugs_to_inject),
        1.96 * math.sqrt(f * (1 - f) / len(bugs_to_inject)))
    )
    print("TESTING COMPLETE")

    if update_db: db.session.commit()

    return real_bugs
