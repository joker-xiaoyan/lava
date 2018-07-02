// This makes sure assertions actually occur.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <set>
#include <memory>
#include <iterator>
#include <stdlib.h>     /* srand, rand */
#include <time.h>       /* time */

extern "C" {
#include <unistd.h>
#include <libgen.h>
}

#include <json/json.h>
#include <odb/pgsql/database.hxx>

#include "clang/AST/AST.h"
#include "clang/Driver/Options.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/Lexer.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Refactoring.h"
#include "clang/Tooling/ReplacementsYaml.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Option/OptTable.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchersInternal.h"
#include "clang/ASTMatchers/ASTMatchersMacros.h"
#include "clang/Lex/Lexer.h"
#include "lavaDB.h"
#include "lava.hxx"
#include "lava-odb.hxx"
#include "lexpr.hxx"
#include "vector_set.hxx"

#define MATCHER (1 << 0)
#define INJECT (1 << 1)
#define FNARG (1 << 2)
#define DEBUG_FLAGS 0 //  (MATCHER | INJECT | FNARG)

#define ARG_NAME "data_flow"

using namespace odb::core;
std::unique_ptr<odb::pgsql::database> db;

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::driver;
using namespace llvm;

using clang::tooling::CommonOptionsParser;
using clang::tooling::SourceFileCallbacks;
using clang::tooling::Replacement;
using clang::tooling::TranslationUnitReplacements;
using clang::tooling::ClangTool;
using clang::tooling::getAbsolutePath;

static cl::OptionCategory
    LavaCategory("LAVA Taint Query and Attack Point Tool Options");
static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);
static cl::extrahelp MoreHelp(
    "\nTODO: Add descriptive help message.  "
    "Automatic clang stuff is ok for now.\n\n");
enum action { LavaQueries, LavaInjectBugs, LavaInstrumentMain };
static cl::opt<action> LavaAction("action", cl::desc("LAVA Action"),
    cl::values(
        clEnumValN(LavaQueries, "query", "Add taint queries"),
        clEnumValN(LavaInjectBugs, "inject", "Inject bugs"),
        clEnumValEnd),
    cl::cat(LavaCategory),
    cl::Required);
static cl::opt<std::string> LavaBugList("bug-list",
    cl::desc("Comma-separated list of bug ids (from the postgres db) to inject into this file"),
    cl::cat(LavaCategory),
    cl::init("XXX"));
static cl::opt<std::string> LavaDB("lava-db",
    cl::desc("Path to LAVA database (custom binary file for source info).  "
        "Created in query mode."),
    cl::cat(LavaCategory),
    cl::init("XXX"));
static cl::opt<std::string> ProjectFile("project-file",
    cl::desc("Path to project.json file."),
    cl::cat(LavaCategory),
    cl::init("XXX"));
static cl::opt<std::string> SourceDir("src-prefix",
    cl::desc("Path to source directory to remove as prefix."),
    cl::cat(LavaCategory),
    cl::init(""));
static cl::opt<std::string> MainFileList("main-files",
    cl::desc("Main files"),
    cl::cat(LavaCategory),
    cl::init(""));
static cl::opt<bool> KnobTrigger("kt",
    cl::desc("Inject in Knob-Trigger style"),
    cl::cat(LavaCategory),
    cl::init(false));
static cl::opt<bool> ArgDataflow("arg_dataflow",
    cl::desc("Use function args for dataflow instead of lava_[sg]et"),
    cl::cat(LavaCategory),
    cl::init(false));
static cl::opt<bool> ArgCompetition("competition",
    cl::desc("Log before/after bugs when competition is #defined"),
    cl::cat(LavaCategory),
    cl::init(false));

std::string LavaPath;

uint32_t num_taint_queries = 0;
uint32_t num_atp_queries = 0;

static llvm::raw_null_ostream null_ostream;
#define debug(flag) ((DEBUG_FLAGS & (flag)) ? llvm::errs() : null_ostream)

Loc::Loc(const FullSourceLoc &full_loc)
    : line(full_loc.getExpansionLineNumber()),
    column(full_loc.getExpansionColumnNumber()) {}

static std::vector<const Bug*> bugs;
static std::set<std::string> main_files;

static std::map<std::string, uint32_t> StringIDs;

// Map of bugs with attack points at a given loc.
std::map<std::pair<LavaASTLoc, AttackPoint::Type>, std::vector<const Bug *>>
    bugs_with_atp_at;

struct LvalBytes {
    const SourceLval *lval;
    Range selected;

    LvalBytes(const SourceLval *lval, Range selected)
        : lval(lval), selected(selected) {}
    LvalBytes(const DuaBytes *dua_bytes)
        : lval(dua_bytes->dua->lval), selected(dua_bytes->selected) {}

    bool operator<(const LvalBytes &other) const {
        return std::tie(lval->id, selected)
            < std::tie(other.lval->id, other.selected);
    }

    friend std::ostream &operator<<(std::ostream &os, const LvalBytes &lval_bytes) {
        os << "LvalBytes " << lval_bytes.selected << " of " << *lval_bytes.lval;
        return os;
    }
};

// Map of bugs with siphon of a given  lval name at a given loc.
std::map<LavaASTLoc, vector_set<LvalBytes>> siphons_at;
std::map<LvalBytes, uint32_t> data_slots;

#define MAX_STRNLEN 64
///////////////// HELPER FUNCTIONS BEGIN ////////////////////
template<typename K, typename V>
const V &map_get_default(const std::map<K, V> &map, K key) {
    static const V default_val;
    auto it = map.find(key);
    if (it != map.end()) {
        return it->second;
    } else {
        return default_val;
    }
}

std::set<std::string> parse_commas_strings(std::string list) {
    std::istringstream ss(list);
    std::set<std::string> result;
    std::string i;
    while(std::getline(ss, i, ',')) {
        result.insert(i);
    }
    return result;
}

template<typename Elem>
std::set<Elem> parse_commas(std::string list) {
    std::istringstream ss(list);
    std::set<Elem> result;
    Elem i;
    while (ss.good()) {
        ss >> i;
        result.insert(i);
        assert(ss.eof() || ss.peek() == ',');
        ss.ignore();
    }
    return result;
}

std::string StripPrefix(std::string filename, std::string prefix) {
    size_t prefix_len = prefix.length();
    if (filename.compare(0, prefix_len, prefix) != 0) {
        printf("Not a prefix!\n");
        assert(false);
    }
    while (filename[prefix_len] == '/') prefix_len++;
    return filename.substr(prefix_len);
}

bool QueriableType(const Type *lval_type) {
    if ((lval_type->isIncompleteType())
        || (lval_type->isIncompleteArrayType())
        || (lval_type->isVoidType())
        || (lval_type->isNullPtrType())
        ) {
        return false;
    }
    if (lval_type->isPointerType()) {
        const Type *pt = lval_type->getPointeeType().getTypePtr();
        return QueriableType(pt);
    }
    return true;
}

bool IsArgAttackable(const Expr *arg) {
    debug(MATCHER) << "IsArgAttackable \n";
    if (DEBUG_FLAGS & MATCHER) arg->dump();

    const Type *t = arg->IgnoreParenImpCasts()->getType().getTypePtr();
    if (dyn_cast<OpaqueValueExpr>(arg) || t->isStructureType() || t->isEnumeralType() || t->isIncompleteType()) {
        return false;
    }
    if (QueriableType(t)) {
        if (t->isPointerType()) {
            const Type *pt = t->getPointeeType().getTypePtr();
            // its a pointer to a non-void
            if ( ! (pt->isVoidType() ) ) {
                return true;
            }
        }
        if ((t->isIntegerType() || t->isCharType()) && (!t->isEnumeralType())) {
            return true;
        }
    }
    return false;
}

///////////////// HELPER FUNCTIONS END ////////////////////

uint32_t Slot(LvalBytes lval_bytes) {
    return data_slots.at(lval_bytes);
}

LExpr Get(LvalBytes x) {
    return (ArgDataflow ? DataFlowGet(Slot(x)) : LavaGet(Slot(x)));
}
LExpr Get(const Bug *bug) { return Get(bug->trigger); }

LExpr Set(LvalBytes x) {
    return (ArgDataflow ? DataFlowSet : LavaSet)(x.lval, x.selected, Slot(x));
}
LExpr Set(const Bug *bug) { return Set(bug->trigger); }

LExpr Test(const Bug *bug) {
    return MagicTest<Get>(bug);
}

LExpr twoDuaTest(const Bug *bug, LvalBytes x) {
    return (Get(bug->trigger)^Get(x)) == LHex(bug->magic);
}

uint32_t rand_ascii4() {
    uint32_t ret = 0;
    for (int i=0; i < 4; i++) {
        ret += (rand() % (0x7F-0x20)) + 0x20;
        if (i !=3) ret = ret<<8;
    }
    return ret;
}

uint32_t alphanum(int len) {
    static const char alphanum[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
    uint32_t ret = 0;
    for (int i=0; i < len; i++) {
        char c = alphanum[rand() % (sizeof(alphanum)-1)];
        ret +=c;
        if (i+1 != len) ret = ret << 8;
    }

    return ret;
}

LExpr threeDuaTest(Bug *bug, LvalBytes x, LvalBytes y) {
        //return (Get(bug->trigger)+Get(x)) == (LHex(bug->magic)*Get(y)); // GOOD
        //return (Get(x)) == (Get(bug->trigger)*(Get(y)+LHex(bug->magic))); // GOOD
        //return (Get(x)%(LHex(bug->magic))) == (LHex(bug->magic) - Get(bug->trigger)); // GOOD

        //return (Get(bug->trigger)<<LHex(3) == (LHex(bug->magic) << LHex(5) + Get(y))); // BAD - segfault
        //return (Get(bug->trigger)^Get(x)) == (LHex(bug->magic)*(Get(y)+LHex(7))); // Segfault

    // TESTING - simple multi dua bug if ABC are all == m we pass
    //return ((Get(x) - Get(y) + Get(bug->trigger)) == LHex(bug->magic));

    // TEST of bug type 2
    //return (Get(x)%(LHex(bug->magic))) == (LHex(bug->magic) - (Get(bug->trigger)*LHex(2)));

    uint32_t a_sol = alphanum(4);
    uint32_t b_sol = alphanum(4);
    uint32_t c_sol = alphanum(4);

    auto oldmagic = bug->magic;

    const int NUM_BUGTYPES=3;
    switch (oldmagic % NUM_BUGTYPES)  {
        case 0:
            bug->magic = (a_sol + b_sol) * c_sol;
            printf("0x%llx == (0x%x + 0x%x) * 0x%x\n", bug->id, a_sol, b_sol, c_sol);
            break;

        case 1:
            bug->magic = (a_sol * b_sol) - c_sol;
            printf("0x%llx id  == (0x%x * 0x%x) - 0x%x\n", bug->id, a_sol, b_sol, c_sol);
            break;

        case 2:
            bug->magic = (a_sol+2) * (b_sol+1) * (c_sol+3);
            printf("0x%llx id == (0x%x+2) *( 0x%x+1) * (0x%x+3) \n", bug->id, a_sol, b_sol, c_sol);
            break;

    }
    //bug->trigger = a_sol;

    switch (oldmagic % NUM_BUGTYPES)  {
        // bug->trigger = A
        // get(x) = B
        // get(y) = C
        // bug->magic = m
        case 0:     // (A + B)*C == M
            return (Get(bug->trigger)+Get(x))*Get(y) == (LHex(bug->magic));
            break;
        case 1:     //(A*B)-C == M
            return (Get(bug->trigger)*Get(x))-Get(y) == (LHex(bug->magic));
            break;
        case 2:     // (A+2)(C+3)(B+1) == M
            return (Get(bug->trigger)+LHex(2))*(Get(y)+LHex(3))*(Get(bug->trigger)+LHex(1))  == LHex(bug->magic);
            break;

        default: // CHAFF
            return (Get(x) == (Get(x)+ LHex(bug->magic)));
            break;
    }
}

LExpr traditionalAttack(const Bug *bug) {
    return Get(bug) * Test(bug);
}


/*
LExpr Test2(const Bug *bug, const DuaBytes *extra) {
    return MagicTest2<Get>(bug, extra);
}*/

LExpr knobTriggerAttack(const Bug *bug) {
    LExpr lava_get_lower = Get(bug) & LHex(0x0000ffff);
    //LExpr lava_get_upper = (LavaGet(bug) >> LDecimal(16)) & LHex(0xffff);
    LExpr lava_get_upper = (Get(bug) & LHex(0xffff0000)) >> LDecimal(16);
    // this is the magic value that will trigger the bug
    // we already know that magic_kt returns uint16_t so we don't have
    // to mask it
    uint16_t magic_value = bug->magic_kt();

    return (lava_get_lower * MagicTest<uint16_t>(magic_value, lava_get_upper))
        + (lava_get_upper * MagicTest<uint16_t>(magic_value, lava_get_lower));
}

/*
 * Keeps track of a list of insertions and makes sure conflicts are resolved.
 */
class Insertions {
private:
    // TODO: use map and "beforeness" concept to robustly avoid duplicate
    // insertions.
    std::map<SourceLocation, std::list<std::string>> impl;

public:
    void clear() { impl.clear(); }

    void InsertAfter(SourceLocation loc, std::string str) {
        if (!str.empty()) {
            std::list<std::string> &strs = impl[loc];
            if (strs.empty() || strs.back() != str || str == ")") {
                impl[loc].push_back(str);
            }
        }
    }

    void InsertBefore(SourceLocation loc, std::string str) {
        if (!str.empty()) {
            std::list<std::string> &strs = impl[loc];
            if (strs.empty() || strs.front() != str || str == "(") {
                impl[loc].push_front(str);
            }
        }
    }

    void render(const SourceManager &sm, std::vector<Replacement> &out) {
        out.reserve(impl.size() + out.size());
        for (const auto &keyvalue : impl) {
            std::stringstream ss;
            for (const std::string &s : keyvalue.second) ss << s;
            out.emplace_back(sm, keyvalue.first, 0, ss.str());
        }
    }
};

/*
 * Contains all the machinery necessary to insert and tries to create some
 * high-level constructs around insertion.
 * Fluent interface to make usage easier. Use Modifier::Change to point at a
 * specific clang expression and the insertion methods to make changes there.
 */
class Modifier {
private:
    const Stmt *stmt = nullptr;

public:
    Insertions &Insert;
    const LangOptions *LangOpts = nullptr;
    const SourceManager *sm = nullptr;

    Modifier(Insertions &Insert) : Insert(Insert) {}

    void Reset(const LangOptions *LangOpts_, const SourceManager *sm_) {
        LangOpts = LangOpts_;
        sm = sm_;
    }

    std::pair<SourceLocation, SourceLocation> range() const {
        auto startRange = sm->getExpansionRange(stmt->getLocStart());
        auto endRange = sm->getExpansionRange(stmt->getLocEnd());
        return std::make_pair(startRange.first, endRange.second);
    }

    SourceLocation before() const {
        return range().first;
    }

    SourceLocation after() const {
        // clang stores ranges as start of first token -> start of last token.
        // so to get character range for replacement, we need to add start of
        // last token.
        SourceLocation end = range().second;
        unsigned lastTokenSize = Lexer::MeasureTokenLength(end, *sm, *LangOpts);
        return end.getLocWithOffset(lastTokenSize);
    }

    const Modifier &InsertBefore(std::string str) const {
        Insert.InsertBefore(before(), str);
        return *this;
    }

    const Modifier &InsertAfter(std::string str) const {
        Insert.InsertAfter(after(), str);
        return *this;
    }

    const Modifier &Change(const Stmt *stmt_) {
        stmt = stmt_;
        return *this;
    }

    const Modifier &Parenthesize() const {
        return InsertBefore("(").InsertAfter(")");
    }

    const Modifier &Operate(std::string op, const LExpr &addend, const Stmt *parent) const {
        InsertAfter(" " + op + " " + addend.render());
        if (parent && !isa<ArraySubscriptExpr>(parent)
                && !isa<ParenExpr>(parent)) {
            Parenthesize();
        }
        return *this;
    }

    const Modifier &Add(const LExpr &addend, const Stmt *parent) const {
        // If inner stmt has lower precedence than addition, add parens.
        const BinaryOperator *binop = dyn_cast<BinaryOperator>(stmt);
        if (isa<AbstractConditionalOperator>(stmt)
                || (binop && !binop->isMultiplicativeOp()
                    && !binop->isAdditiveOp())) {
            Parenthesize();
        }
        return Operate("+", addend, parent);
    }

    void InsertAt(SourceLocation loc, std::string str) {
        Insert.InsertBefore(loc, str);
    }
};

/*******************************
 * Matcher Handlers
 *******************************/
struct LavaMatchHandler : public MatchFinder::MatchCallback {
    LavaMatchHandler(Modifier &Mod) : Mod(Mod) {}

    std::string ExprStr(const Stmt *e) {
        clang::PrintingPolicy Policy(*LangOpts);
        std::string TypeS;
        llvm::raw_string_ostream s(TypeS);
        e->printPretty(s, 0, Policy);
        return s.str();
    }

    LavaASTLoc GetASTLoc(const SourceManager &sm, const Stmt *s) {
        assert(!SourceDir.empty());
        FullSourceLoc fullLocStart(sm.getExpansionLoc(s->getLocStart()), sm);
        FullSourceLoc fullLocEnd(sm.getExpansionLoc(s->getLocEnd()), sm);
        std::string src_filename = StripPrefix(
                getAbsolutePath(sm.getFilename(fullLocStart)), SourceDir);
        return LavaASTLoc(src_filename, fullLocStart, fullLocEnd);
    }

    LExpr LavaAtpQuery(LavaASTLoc ast_loc, AttackPoint::Type atpType) {
        return LBlock({
                LFunc("vm_lava_attack_point2",
                    { LDecimal(GetStringID(StringIDs, ast_loc)), LDecimal(0),
                        LDecimal(atpType) }),
                LDecimal(0) });
    }

    void AttackExpression(const SourceManager &sm, const Expr *toAttack,
            const Expr *parent, const Expr *rhs, AttackPoint::Type atpType) {
        LavaASTLoc ast_loc = GetASTLoc(sm, toAttack);
        std::vector<LExpr> pointerAddends;
        std::vector<LExpr> valueAddends;
        std::vector<LExpr> triggers;
        std::vector<Bug*> bugs;

        //debug(INJECT) << "Inserting expression attack (AttackExpression).\n";
        const Bug *this_bug = NULL;
        if (LavaAction == LavaInjectBugs) {
            const std::vector<const Bug*> &injectable_bugs =
                map_get_default(bugs_with_atp_at,
                        std::make_pair(ast_loc, atpType));

            if (injectable_bugs.size() == 0 && ArgCompetition) return;

            // this should be a function bug -> LExpr to add.
            auto pointerAttack = KnobTrigger ? knobTriggerAttack : traditionalAttack;
            for (const Bug *bug : injectable_bugs) {
                assert(bug->atp->type == atpType);
                // was in if ArgCompetition, but we want to inject bugs more often
                Bug *bug2 = NULL;
                bug2 = (Bug*)malloc(sizeof(Bug));
                memcpy(bug2, bug, sizeof(Bug));
                bugs.push_back(bug2);

                if (bug->type == Bug::PTR_ADD) {
                    pointerAddends.push_back(pointerAttack(bug));
                    triggers.push_back(Test(bug)); //  Might fail for knobTriggers?
                } else if (bug->type == Bug::REL_WRITE) {
                    const DuaBytes *extra0 = db->load<DuaBytes>(bug2->extra_duas[0]);
                    const DuaBytes *extra1 = db->load<DuaBytes>(bug2->extra_duas[1]);
                    auto bug_combo = threeDuaTest(bug2, extra0, extra1); // Non-deterministic, need one object for triggers and ptr addends
                    triggers.push_back(bug_combo);

                    pointerAddends.push_back(bug_combo * Get(extra0));
                }
            }
            bugs_with_atp_at.erase(std::make_pair(ast_loc, atpType));
        } else if (LavaAction == LavaQueries) {
            // call attack point hypercall and return 0
            pointerAddends.push_back(LavaAtpQuery(ast_loc, atpType));
            num_atp_queries++;
        }


        if (!pointerAddends.empty()) {
            LExpr addToPointer = LBinop("+", std::move(pointerAddends));
            Mod.Change(toAttack).Add(addToPointer, parent);

            // For competitions, wrap pointer value in LAVALOG macro call-
            // it's effectively just a NOP that prints a message when the trigger is true
            // so we can identify when bugs are potentially triggered
            if (ArgCompetition) {
                assert (triggers.size() == bugs.size());

                for (int i=0; i < triggers.size(); i++) {
                    Bug *bug = bugs[i];
                    std::stringstream start_str;
                    start_str << "LAVALOG(" << bug->id << ", ";
                    Mod.Change(toAttack).InsertBefore(start_str.str());

                    std::stringstream end_str;

                    end_str << ", " << triggers[i] << ")";
                    Mod.Change(toAttack).InsertAfter(end_str.str());
                    free(bug);
                }
            }
        }

        /*
        if (!valueAddends.empty()) {
            assert(rhs);
            LExpr addToValue = LBinop("+", std::move(valueAddends));
            Mod.Change(rhs).Add(addToValue, nullptr);
        }
        */
    }

    virtual void handle(const MatchFinder::MatchResult &Result) = 0;
    virtual ~LavaMatchHandler() = default;

    virtual void run(const MatchFinder::MatchResult &Result) {
        const SourceManager &sm = *Result.SourceManager;
        auto nodesMap = Result.Nodes.getMap();

        debug(MATCHER) << "====== Found Match =====\n";
        for (auto &keyValue : nodesMap) {
            const Stmt *stmt = keyValue.second.get<Stmt>();
            if (stmt) {
                SourceLocation start = stmt->getLocStart();
                if (!sm.getFilename(start).empty() && sm.isInMainFile(start)
                        && !sm.isMacroArgExpansion(start)) {
                    debug(MATCHER) << keyValue.first << ": " << ExprStr(stmt) << " ";
                    stmt->getLocStart().print(debug(MATCHER), sm);
                    debug(MATCHER) << "\n";
                    if (DEBUG_FLAGS & MATCHER) stmt->dump();
                } else return;
            }
        }
        handle(Result);
    }

    const LangOptions *LangOpts = nullptr;

protected:
    Modifier &Mod;
};

struct PriQueryPointHandler : public LavaMatchHandler {
    using LavaMatchHandler::LavaMatchHandler; // Inherit constructor.

    // create code that siphons dua bytes into a global
    // for dua x, offset o, generates:
    // lava_set(slot, *(const unsigned int *)(((const unsigned char *)x)+o)
    // Each lval gets an if clause containing one siphon
    std::string SiphonsForLocation(LavaASTLoc ast_loc) {
        std::stringstream result_ss;
        for (const LvalBytes &lval_bytes : map_get_default(siphons_at, ast_loc)) {
            result_ss << LIf(lval_bytes.lval->ast_name, Set(lval_bytes));
        }

        std::string result = result_ss.str();
        if (!result.empty()) {
            debug(INJECT) << " Injecting dua siphon at " << ast_loc << "\n";
            debug(INJECT) << "    Text: " << result << "\n";
        }
        siphons_at.erase(ast_loc); // Only inject once.
        return result;
    }

    std::string AttackRetBuffer(LavaASTLoc ast_loc) {
        std::stringstream result_ss;
        auto key = std::make_pair(ast_loc, AttackPoint::QUERY_POINT);
        for (const Bug *bug : map_get_default(bugs_with_atp_at, key)) {
            if (bug->type == Bug::RET_BUFFER) {
                const DuaBytes *buffer = db->load<DuaBytes>(bug->extra_duas[0]);
                if (ArgCompetition) {
                    result_ss << LIf(Test(bug).render(), {
                            LBlock({
                                LFunc("LAVALOG", {LDecimal(bug->id), LDecimal(1), LDecimal(1)}), //It's always safe to call lavalog here since we're in the if
                                LIfDef("__x86_64__", {
                                    LAsm({ UCharCast(LStr(buffer->dua->lval->ast_name)) +
                                        LDecimal(buffer->selected.low), },
                                        { "movq %0, %%rsp", "ret" }),
                                    LAsm({ UCharCast(LStr(buffer->dua->lval->ast_name)) +
                                        LDecimal(buffer->selected.low), },
                                        { "movl %0, %%esp", "ret" })})})});
                }else{
                    result_ss << LIf(Test(bug).render(), {
                                LIfDef("__x86_64__", {
                                    LAsm({ UCharCast(LStr(buffer->dua->lval->ast_name)) +
                                        LDecimal(buffer->selected.low), },
                                        { "movq %0, %%rsp", "ret" }),
                                    LAsm({ UCharCast(LStr(buffer->dua->lval->ast_name)) +
                                        LDecimal(buffer->selected.low), },
                                        { "movl %0, %%esp", "ret" })})});
                }
            }
        }
        bugs_with_atp_at.erase(key); // Only inject once.
        return result_ss.str();
    }

    virtual void handle(const MatchFinder::MatchResult &Result) override {
        const Stmt *toSiphon = Result.Nodes.getNodeAs<Stmt>("stmt");
        const SourceManager &sm = *Result.SourceManager;

        LavaASTLoc ast_loc = GetASTLoc(sm, toSiphon);
        //debug(INJECT) << "Have a query point @ " << ast_loc << "!\n";

        std::string before;
        if (LavaAction == LavaQueries) {
            before = "; " + LFunc("vm_lava_pri_query_point", {
                LDecimal(GetStringID(StringIDs, ast_loc)),
                LDecimal(ast_loc.begin.line),
                LDecimal(0)}).render() + "; ";

            num_taint_queries += 1;
        } else if (LavaAction == LavaInjectBugs) {
            before = SiphonsForLocation(ast_loc) + AttackRetBuffer(ast_loc);
        }
        Mod.Change(toSiphon).InsertBefore(before);
    }
};

struct FunctionArgHandler : public LavaMatchHandler {
    using LavaMatchHandler::LavaMatchHandler; // Inherit constructor.

    virtual void handle(const MatchFinder::MatchResult &Result) override {
        const Expr *toAttack = Result.Nodes.getNodeAs<Expr>("arg");
        const SourceManager &sm = *Result.SourceManager;

        //debug(INJECT) << "FunctionArgHandler @ " << GetASTLoc(sm, toAttack) << "\n";

        AttackExpression(sm, toAttack, nullptr, nullptr, AttackPoint::FUNCTION_ARG);
    }
};

struct ReadDisclosureHandler : public LavaMatchHandler {
    using LavaMatchHandler::LavaMatchHandler; // Inherit constructor.

    virtual void handle(const MatchFinder::MatchResult &Result) {
        const SourceManager &sm = *Result.SourceManager;
        const CallExpr *callExpr = Result.Nodes.getNodeAs<CallExpr>("call_expression");

        LExpr addend = LDecimal(0);
        // iterate through all the arguments in the call expression
        for (auto it = callExpr->arg_begin(); it != callExpr->arg_end(); ++it) {
            const Expr *arg = dyn_cast<Expr>(*it);
            if (arg) {
                if (arg->IgnoreImpCasts()->isLValue() && arg->getType()->isIntegerType()) {
                    LavaASTLoc ast_loc = GetASTLoc(sm, arg);
                    Mod.Change(arg);
                    if (LavaAction == LavaQueries)  {
                        addend = LavaAtpQuery(GetASTLoc(sm, arg),
                                AttackPoint::PRINTF_LEAK);
                        Mod.Add(addend, nullptr);
                    } else if (LavaAction == LavaInjectBugs) {
                        const std::vector<const Bug*> &injectable_bugs =
                            map_get_default(bugs_with_atp_at,
                                    std::make_pair(ast_loc, AttackPoint::PRINTF_LEAK));
                        for (const Bug *bug : injectable_bugs) {
                            Mod.Parenthesize()
                                .InsertBefore(Test(bug).render() +
                                        " ? &(" + ExprStr(arg) + ") : ");
                        }
                    }
                }
            }
        }
    }
};

struct MemoryAccessHandler : public LavaMatchHandler {
    using LavaMatchHandler::LavaMatchHandler; // Inherit constructor.

    virtual void handle(const MatchFinder::MatchResult &Result) override {
        const Expr *toAttack = Result.Nodes.getNodeAs<Expr>("innerExpr");
        const Expr *parent = Result.Nodes.getNodeAs<Expr>("lhs");
        const SourceManager &sm = *Result.SourceManager;
        LavaASTLoc ast_loc = GetASTLoc(sm, toAttack);
        //debug(INJECT) << "PointerAtpHandler @ " << ast_loc << "\n";

        const Expr *rhs = nullptr;
        AttackPoint::Type atpType = AttackPoint::POINTER_READ;

        // memwrite style attack points will have rhs bound to a node
        auto it = Result.Nodes.getMap().find("rhs");
        if (it != Result.Nodes.getMap().end()){
            atpType = AttackPoint::POINTER_WRITE;
            rhs = it->second.get<Expr>();
            assert(rhs);
        }

        AttackExpression(sm, toAttack, parent, rhs, atpType);
    }
};

// getText is from https://github.com/LegalizeAdulthood/remove-void-args
// TODO this can probably be reimplemented as a part of the main lavaTool with everything else
template <typename T>
static std::string getText(const SourceManager &SourceManager, const T &Node) {
  SourceLocation StartSpellingLocation =
      SourceManager.getSpellingLoc(Node.getLocStart());
  SourceLocation EndSpellingLocation =
      SourceManager.getSpellingLoc(Node.getLocEnd());
  if (!StartSpellingLocation.isValid() || !EndSpellingLocation.isValid()) {
    return std::string();
  }
  bool Invalid = true;
  const char *Text =
      SourceManager.getCharacterData(StartSpellingLocation, &Invalid);
  if (Invalid) {
    return std::string();
  }
  std::pair<FileID, unsigned> Start =
      SourceManager.getDecomposedLoc(StartSpellingLocation);
  std::pair<FileID, unsigned> End =
      SourceManager.getDecomposedLoc(Lexer::getLocForEndOfToken(
          EndSpellingLocation, 0, SourceManager, LangOptions()));
  if (Start.first != End.first) {
    // Start and end are in different files.
    return std::string();
  }
  if (End.second < Start.second) {
    // Shuffling text with macros may cause this.
    return std::string();
  }
  return std::string(Text, End.second - Start.second);
}

// FixVoidArg is from https://github.com/LegalizeAdulthood/remove-void-args
namespace {
class FixVoidArg : public ast_matchers::MatchFinder::MatchCallback {
 public:
  FixVoidArg(tooling::Replacements *Replace)
      : Replace(Replace) {}

  virtual void run(const ast_matchers::MatchFinder::MatchResult &Result) {
    BoundNodes Nodes = Result.Nodes;
    SourceManager const *SM = Result.SourceManager;
    if (FunctionDecl const *const Function = Nodes.getNodeAs<FunctionDecl>("fn")) {
        /*
        if (Function->isExternC()) {
            return;
        }
        */
        std::string const Text = getText(*SM, *Function);
        if (!Function->isThisDeclarationADefinition()) {
            if (Text.length() > 6 && Text.substr(Text.length()-6) == "(void)") {
                std::string const NoVoid = Text.substr(0, Text.length()-6) + "()";
                Replace->insert(Replacement(*Result.SourceManager, Function, NoVoid));
            }
        } else if (Text.length() > 0) {
            std::string::size_type EndOfDecl = Text.find_last_of(')', Text.find_first_of('{')) + 1;
            std::string Decl = Text.substr(0, EndOfDecl);
            if (Decl.length() > 6 && Decl.substr(Decl.length()-6) == "(void)") {
                std::string NoVoid = Decl.substr(0, Decl.length()-6) + "()" + Text.substr(EndOfDecl);
                Replace->insert(Replacement(*Result.SourceManager, Function, NoVoid));
            }
        }
    }
  }

 private:
  tooling::Replacements *Replace;
};
}

/*
struct MacroDeclArgAdditionHandler : public MatchFinder::MatchCallback {
    MacroDeclArgAdditionHandler(Modifier &Mod) : Mod(Mod) {}

    virtual void run(const MatchFinder::MatchResult &Result) {
        const SourceManager &sm = *Result.SourceManager;
        auto nodesMap = Result.Nodes.getMap();

        debug(MATCHER) << "====== Found MACRO DECL Match =====\n";
        for (auto &keyValue : nodesMap) {
            const Stmt *stmt = keyValue.second.get<Stmt>();
            if (stmt) {
                SourceLocation start = stmt->getLocStart();
                if (!sm.getFilename(start).empty() && sm.isInMainFile(start)
                        && !sm.isMacroArgExpansion(start)) {
                    debug(MATCHER) << keyValue.first << ": " << ExprStr(stmt) << " ";
                    stmt->getLocStart().print(debug(MATCHER), sm);
                    debug(MATCHER) << "\n";
                    if (DEBUG_FLAGS & MATCHER) stmt->dump();
                } else return;
            }
        }
        //handle(Result); // TODO
    }

    void AddArg(const FunctionDecl *func) {
        SourceLocation loc = clang::Lexer::findLocationAfterToken(
                func->getLocation(), tok::l_paren, *Mod.sm, *Mod.LangOpts, true);

        if (func->getNumParams() == 0) {
            // Foo(void) is considered to have 0 params which can lead to `foo(int *data_flowvoid)`
            // This is fixed by always running FixVoidArg before we get here
            Mod.InsertAt(loc, "int *" ARG_NAME);
        } else {
            Mod.InsertAt(loc, "int *" ARG_NAME ", ");
        }
    }

    virtual void handle(const MatchFinder::MatchResult &Result) {
        const FunctionDecl *func =
            Result.Nodes.getNodeAs<FunctionDecl>("funcDecl");

        debug(FNARG) << "adding arg to " << func->getNameAsString() << "\n";

        if (func->isThisDeclarationADefinition()) debug(FNARG) << "has body\n";
        if (func->getBody()) debug(FNARG) << "can find body\n";

        if (func->getLocation().isInvalid()) return;
        if (func->getNameAsString().find("lava") == 0) return;
        if (Mod.sm->isInSystemHeader(func->getLocation())) return;
        if (Mod.sm->getFilename(func->getLocation()).empty()) return;

        // Comment out format attrs
        if (func->hasAttrs()) {
          auto attrs = func->getAttrs();
          for (const auto &a : func->getAttrs()) {
            if (a->getKind() == attr::Format) {
              debug(FNARG) << "found format attr\n";
              Mod.InsertAt(a->getRange().getBegin(), ")); //");
            }
            debug(FNARG) << a->getSpelling() << "\n";
          }
        }

        debug(FNARG) << "actually adding arg\n";

        if (func->isMain()) {
            if (func->isThisDeclarationADefinition()) { // no prototype for main.
                CompoundStmt *body = dyn_cast<CompoundStmt>(func->getBody());
                assert(body);
                Stmt *first = *body->body_begin();
                assert(first);

                std::stringstream data_array;
                data_array << "int data[" << data_slots.size() << "] = {0};\n";
                data_array << "int *" ARG_NAME << "= &data;\n";
                Mod.InsertAt(first->getLocStart(), data_array.str());
            }
        } else {
            const FunctionDecl *bodyDecl = nullptr;
            func->hasBody(bodyDecl);
        }
        return;
    }
protected:
    Modifier &Mod;
};
*/

struct FuncDeclArgAdditionHandler : public LavaMatchHandler {
    using LavaMatchHandler::LavaMatchHandler; // Inherit constructor

    void AddArg(const FunctionDecl *func) {
        SourceLocation loc = clang::Lexer::findLocationAfterToken(
                func->getLocation(), tok::l_paren, *Mod.sm, *Mod.LangOpts, true);

        if (func->getNumParams() == 0) {
            // Foo(void) is considered to have 0 params which can lead to `foo(int *data_flowvoid)`
            // This is fixed by always running FixVoidArg before we get here
            Mod.InsertAt(loc, "int *" ARG_NAME);
        } else {
            Mod.InsertAt(loc, "int *" ARG_NAME ", ");
        }
    }

    virtual void handle(const MatchFinder::MatchResult &Result) {
        const FunctionDecl *func =
            Result.Nodes.getNodeAs<FunctionDecl>("funcDecl");

        debug(FNARG) << "adding arg to " << func->getNameAsString() << "\n";

        if (func->isThisDeclarationADefinition()) debug(FNARG) << "has body\n";
        if (func->getBody()) debug(FNARG) << "can find body\n";

        if (func->getLocation().isInvalid()) return;
        if (func->getNameAsString().find("lava") == 0) return;
        if (Mod.sm->isInSystemHeader(func->getLocation())) return;
        if (Mod.sm->getFilename(func->getLocation()).empty()) return;

        // Comment out format attrs
        if (func->hasAttrs()) {
          auto attrs = func->getAttrs();
          for (const auto &a : func->getAttrs()) {
            if (a->getKind() == attr::Format) {
              debug(FNARG) << "found format attr\n";
              Mod.InsertAt(a->getRange().getBegin(), ")); //");
            }
            debug(FNARG) << a->getSpelling() << "\n";
          }
        }

        debug(FNARG) << "actually adding arg\n";

        if (func->isMain()) {
            if (func->isThisDeclarationADefinition()) { // no prototype for main.
                CompoundStmt *body = dyn_cast<CompoundStmt>(func->getBody());
                assert(body);
                Stmt *first = *body->body_begin();
                assert(first);

                std::stringstream data_array;
                data_array << "int data[" << data_slots.size() << "] = {0};\n";
                data_array << "int *" ARG_NAME << "= &data;\n";
                Mod.InsertAt(first->getLocStart(), data_array.str());
            }
        } else {
            const FunctionDecl *bodyDecl = nullptr;
            func->hasBody(bodyDecl);
            if (bodyDecl) AddArg(bodyDecl);
            while (func != NULL) {
                AddArg(func);
                func = func->getPreviousDecl();
                if (func) debug(FNARG) << "found a redeclaration\n";
            }
        }
        return;
    }
};

struct FunctionPointerFieldHandler : public LavaMatchHandler {
    using LavaMatchHandler::LavaMatchHandler; // Inherit constructor.

    virtual void handle(const MatchFinder::MatchResult &Result) {
        const FieldDecl *decl = Result.Nodes.getNodeAs<FieldDecl>("fieldDecl");
        debug(FNARG) << decl->getLocEnd().printToString(*Mod.sm) << "\n";
        Mod.InsertAt(decl->getLocEnd().getLocWithOffset(-14), "int *" ARG_NAME ", ");
    }
};

struct CallExprArgAdditionHandler : public LavaMatchHandler {
    using LavaMatchHandler::LavaMatchHandler; // Inherit constructor.

    virtual void handle(const MatchFinder::MatchResult &Result) {
        const CallExpr *call = Result.Nodes.getNodeAs<CallExpr>("callExpr");
        const FunctionDecl *func = call->getDirectCallee();
        SourceLocation loc = clang::Lexer::findLocationAfterToken(
                call->getLocStart(), tok::l_paren, *Mod.sm, *Mod.LangOpts, true);

        if (func == nullptr || func->getLocation().isInvalid()) {
            // Function Pointer
            debug(FNARG) << "FUNCTION POINTER USE: ";
            call->getLocStart().print(debug(FNARG), *Mod.sm);
            debug(FNARG) << "this many args: " << call->getNumArgs() << "\n";
            loc = call->getArg(0)->getLocStart();
        } else if (Mod.sm->isInSystemHeader(func->getLocation())) {
            return;
        }

        loc.print(debug(FNARG), *Mod.sm);

        if (call->getNumArgs() == 0) {
            Mod.InsertAt(loc, ARG_NAME);
        } else {
            Mod.InsertAt(loc, ARG_NAME ", ");
        }
    }
};

namespace clang {
    namespace ast_matchers {
        AST_MATCHER(Expr, isAttackableMatcher){
            const Expr *ce = &Node;
            return IsArgAttackable(ce);
        }

        AST_MATCHER(VarDecl, isStaticLocalDeclMatcher){
            const VarDecl *vd = &Node;
            return vd->isStaticLocal();
        }

        AST_MATCHER_P(CallExpr, forEachArgMatcher,
                internal::Matcher<Expr>, InnerMatcher) {
            BoundNodesTreeBuilder Result;
            bool Matched = false;
            for ( const auto *I : Node.arguments()) {
                //for (const auto *I : Node.inits()) {
                BoundNodesTreeBuilder InitBuilder(*Builder);
                if (InnerMatcher.matches(*I, Finder, &InitBuilder)) {
                    Matched = true;
                    Result.addMatch(InitBuilder);
                }
            }
            *Builder = std::move(Result);
            return Matched;
        }
    }
}

class LavaMatchFinder : public MatchFinder, public SourceFileCallbacks {
public:
    LavaMatchFinder() : Mod(Insert) {
        StatementMatcher memoryAccessMatcher =
            allOf(
                expr(anyOf(
                    arraySubscriptExpr(
                        hasIndex(ignoringImpCasts(
                                expr().bind("innerExpr")))),
                    unaryOperator(hasOperatorName("*"),
                        hasUnaryOperand(ignoringImpCasts(
                                expr().bind("innerExpr")))))).bind("lhs"),
                anyOf(
                    expr(hasAncestor(binaryOperator(allOf(
                                    hasOperatorName("="),
                                    hasRHS(ignoringImpCasts(
                                            expr().bind("rhs"))),
                                    hasLHS(ignoringImpCasts(expr(
                                                equalsBoundNode("lhs")))))))),
                    anything()), // this is a "maybe" construction.
                hasAncestor(functionDecl()), // makes sure that we are't in a global variable declaration
                // make sure we aren't in static local variable initializer which must be constant
                unless(hasAncestor(varDecl(isStaticLocalDeclMatcher()))));

        addMatcher(
                stmt(hasParent(compoundStmt())).bind("stmt"),
                makeHandler<PriQueryPointHandler>()
                );

        addMatcher(
                callExpr(
                    forEachArgMatcher(expr(isAttackableMatcher()).bind("arg"))),
                makeHandler<FunctionArgHandler>()
                );

        addMatcher(memoryAccessMatcher, makeHandler<MemoryAccessHandler>());

        // fortenforge's matchers (for argument addition)
        if (ArgDataflow && LavaAction == LavaInjectBugs) {
            // Function declaration
            addMatcher(
                    functionDecl().bind("funcDecl"),
                    makeHandler<FuncDeclArgAdditionHandler>());

            // Function call
            addMatcher(
                    callExpr().bind("callExpr"),
                    makeHandler<CallExprArgAdditionHandler>());

            // Macro declaration TODO
            // TODO not finding any matches
            /*addMatcher(
                    fieldDecl().bind("fieldDecl"), // TODO add filter to ensure is macro
                    new MacroDeclArgAdditionHandler(Mod));*/

            // Macro call TODO

            // Struct
            addMatcher(
                    fieldDecl(anyOf(hasName("as_number"), hasName("as_string"))).bind("fieldDecl"),
                    makeHandler<FunctionPointerFieldHandler>());
        }

        addMatcher(
                callExpr(
                    callee(functionDecl(hasName("::printf"))),
                    unless(argumentCountIs(1))).bind("call_expression"),
                makeHandler<ReadDisclosureHandler>()
                );
        }
    virtual bool handleBeginSource(CompilerInstance &CI, StringRef Filename) override {
        Insert.clear();
        Mod.Reset(&CI.getLangOpts(), &CI.getSourceManager());
        TUReplace.Replacements.clear();
        TUReplace.MainSourceFile = Filename;
        CurrentCI = &CI;

        debug(INJECT) << "*** handleBeginSource for: " << Filename << "\n";

        std::stringstream logging_macros;
        logging_macros << "#include <stdio.h>\n" // enable logging with (LAVA_LOGGING, FULL_LAVA_LOGGING) and (DUA_LOGGING) flags
                    << "#ifdef LAVA_LOGGING\n"
                        << "#define LAVALOG(bugid, x, trigger)  ({(trigger && fprintf(stderr, \"\\nLAVALOG: %d: %s:%d\\n\", bugid, __FILE__, __LINE__)), (x);})\n"
                    << "#endif\n"

                    << "#ifdef FULL_LAVA_LOGGING\n"
                        << "#define LAVALOG(bugid, x, trigger)  ({(trigger && fprintf(stderr, \"\\nLAVALOG: %d: %s:%d\\n\", bugid, __FILE__, __LINE__), (!trigger && fprintf(stderr, \"\\nLAVALOG_MISS: %d: %s:%d\\n\", bugid, __FILE__, __LINE__))) && fflush(NULL), (x);})\n"
                    << "#endif\n"

                    << "#ifndef LAVALOG\n"
                        << "#define LAVALOG(y,x,z)  (x)\n"
                    << "#endif\n"

                    << "#ifdef DUA_LOGGING\n"
                        << "#define DFLOG(idx, val)  ({fprintf(stderr, \"\\nDFLOG:%d=%d: %s:%d\\n\", idx, val, __FILE__, __LINE__) && fflush(NULL), data_flow[idx]=val;})\n"
                    << "#else\n"
                        << "#define DFLOG(idx, val) {data_flow[idx]=val;}\n"
                    << "#endif\n";

        std::string insert_at_top;
        if (LavaAction == LavaQueries) {
            insert_at_top = "#include \"pirate_mark_lava.h\"\n";
        } else if (LavaAction == LavaInjectBugs) {
            insert_at_top.append(logging_macros.str());
            if (!ArgDataflow) {
                if (main_files.count(getAbsolutePath(Filename)) > 0) {
                    std::stringstream top;
                    top << "static unsigned int lava_val[" << data_slots.size() << "] = {0};\n"
                        << "void lava_set(unsigned int, unsigned int);\n"
                        << "__attribute__((visibility(\"default\")))\n"
                        << "void lava_set(unsigned int slot, unsigned int val) {\n"
                        << "#ifdef DUA_LOGGING\n"
                            << "fprintf(stderr, \"\\nlava_set:%d=%d: %s:%d\\n\", slot, val, __FILE__, __LINE__);\n"
                            << "fflush(NULL);\n"
                        << "#endif\n"
                        << "lava_val[slot] = val; }\n"
                        << "unsigned int lava_get(unsigned int);\n"
                        << "__attribute__((visibility(\"default\")))\n"
                        << "unsigned int lava_get(unsigned int slot) { return lava_val[slot]; }\n";
                    insert_at_top.append(top.str());
                } else {
                    insert_at_top.append("void lava_set(unsigned int bn, unsigned int val);\n"
                    "extern unsigned int lava_get(unsigned int);\n");
                }
            }
        }

        debug(INJECT) << "Inserting Macros and lava_set/get or dataflow at top of file\n";
        TUReplace.Replacements.emplace_back(Filename, 0, 0, insert_at_top);

        for (auto it = MatchHandlers.begin();
                it != MatchHandlers.end(); it++) {
            (*it)->LangOpts = &CI.getLangOpts();
        }

        return true;
    }

    virtual void handleEndSource() override {
        debug(INJECT) << "*** handleEndSource\n";

        Insert.render(CurrentCI->getSourceManager(), TUReplace.Replacements);
        std::error_code EC;
        llvm::raw_fd_ostream YamlFile(TUReplace.MainSourceFile + ".yaml",
                EC, llvm::sys::fs::F_RW);
        yaml::Output Yaml(YamlFile);
        Yaml << TUReplace;
    }

    template<class Handler>
    LavaMatchHandler *makeHandler() {
        MatchHandlers.emplace_back(new Handler(Mod));
        return MatchHandlers.back().get();
    }

private:
    Insertions Insert;
    Modifier Mod;
    TranslationUnitReplacements TUReplace;
    std::vector<std::unique_ptr<LavaMatchHandler>> MatchHandlers;
    CompilerInstance *CurrentCI = nullptr;
};

void mark_for_siphon(const DuaBytes *dua_bytes) {

    LvalBytes lval_bytes(dua_bytes);
    siphons_at[lval_bytes.lval->loc].insert(lval_bytes);

    debug(INJECT) << "    Mark siphon at " << lval_bytes.lval->loc << "\n";

    // if insert fails do nothing. we already have a slot for this one.
    data_slots.insert(std::make_pair(lval_bytes, data_slots.size()));
}

int main(int argc, const char **argv) {
    std::cout << "Starting lavaTool...\n";
    CommonOptionsParser op(argc, argv, LavaCategory);

    LavaPath = std::string(dirname(dirname(dirname(realpath(argv[0], NULL)))));
    srand(time(NULL));


    std::ifstream json_file(ProjectFile);
    Json::Value root;
    if (ProjectFile == "XXX") {
        if (LavaAction == LavaInjectBugs) {
            errs() << "Error: Specify a json file with \"-project-file\".  Exiting . . .\n";
            exit(1);
        }
    } else {
        json_file >> root;
    }

    if (LavaDB != "XXX") StringIDs = LoadDB(LavaDB);

    odb::transaction *t = nullptr;
    if (LavaAction == LavaInjectBugs) {
        db.reset(new odb::pgsql::database("postgres", "postgrespostgres",
                    root["db"].asString()));
        t = new odb::transaction(db->begin());

        main_files = parse_commas_strings(MainFileList);

        // get bug info for the injections we are supposed to be doing.
        debug(INJECT) << "LavaBugList: [" << LavaBugList << "]\n";

        std::set<uint32_t> bug_ids = parse_commas<uint32_t>(LavaBugList);
        // for each bug_id, load that bug from DB and insert into bugs vector.
        std::transform(bug_ids.begin(), bug_ids.end(), std::back_inserter(bugs),
                [&](uint32_t bug_id) { return db->load<Bug>(bug_id); });

        for (const Bug *bug : bugs) {
            LavaASTLoc atp_loc = bug->atp->loc;
            auto key = std::make_pair(atp_loc, bug->atp->type);
            bugs_with_atp_at[key].push_back(bug);

            mark_for_siphon(bug->trigger);

            //if (bug->type != Bug::RET_BUFFER) {
                for (uint64_t dua_id : bug->extra_duas) {
                    const DuaBytes *dua_bytes = db->load<DuaBytes>(dua_id);
                    mark_for_siphon(dua_bytes);
                }
            //}
        }
    }

    // Remove void arguments if we're using dataflow // TODO this doesn't work with file?
    /*
    if (ArgDataflow && LavaAction == LavaInjectBugs) {
        tooling::RefactoringTool refTool(op.getCompilations(), op.getSourcePathList());
        ast_matchers::MatchFinder Finder;
        FixVoidArg Callback(&refTool.getReplacements());
        Finder.addMatcher(functionDecl(parameterCountIs(0)).bind("fn"), &Callback);
        debug(INJECT) << "about to call FixVoidArg Tool.run\n";
        refTool.runAndSave(clang::tooling::newFrontendActionFactory(&Finder).get());
        debug(INJECT) << "back from FixVoidArg Tool.run\n";
    }
    */

    // Create tool after we already have fixed void args
    ClangTool Tool(op.getCompilations(), op.getSourcePathList());
    debug(INJECT) << "about to call Tool.run \n";
    LavaMatchFinder Matcher;
    Tool.run(newFrontendActionFactory(&Matcher, &Matcher).get());
    debug(INJECT) << "back from calling Tool.run \n";

    if (LavaAction == LavaQueries) {
        std::cout << "num taint queries added " << num_taint_queries << "\n";
        std::cout << "num atp queries added " << num_atp_queries << "\n";

        if (LavaDB != "XXX") SaveDB(StringIDs, LavaDB);
    } else if (LavaAction == LavaInjectBugs) {
        if (!bugs_with_atp_at.empty()) {
            std::cout << "Warning: Failed to inject attacks for bugs:\n";
            for (const auto &keyvalue : bugs_with_atp_at) {
                std::cout << "    At " << keyvalue.first.first << "\n";
                for (const Bug *bug : keyvalue.second) {
                    std::cout << "        " << *bug << "\n";
                }
            }

            std::cout << "Failed bugs: ";
            for (const auto &keyvalue : bugs_with_atp_at) {
                for (const Bug *bug : keyvalue.second) {
                    std::cout << bug->id << ",";
                }
            }
            std::cout << std::endl;
        }
        if (!siphons_at.empty()) {
            std::cout << "Warning: Failed to inject siphons:\n";
            for (const auto &keyvalue : siphons_at) {
                std::cout << "    At " << keyvalue.first << "\n";
                for (const LvalBytes &lval_bytes : keyvalue.second) { // TODO print failed bugs for siphons as well
                    std::cout << "        " << lval_bytes << "\n";
                }
            }
        }
    }

    if (t) {
        t->commit();
        delete t;
    }

    return 0;
}
