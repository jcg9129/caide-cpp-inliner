//                        Caide C++ inliner
//
// This file is distributed under the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or (at your
// option) any later version. See LICENSE.TXT for details.

#include "optimizer.h"
#include "DependenciesCollector.h"
#include "MergeNamespacesVisitor.h"
#include "OptimizerVisitor.h"
#include "RemoveInactivePreprocessorBlocks.h"
#include "SmartRewriter.h"
#include "SourceInfo.h"
#include "util.h"
#include "Timer.h"

// #define CAIDE_DEBUG_MODE
#include "caide_debug.h"
#include "clang_version.h"


#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Sema/Sema.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Tooling.h>


#include <fstream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>


using namespace clang;
using std::set;
using std::string;
using std::vector;


namespace caide {
namespace internal {

// The 'optimizer' stage acts on a single source file without dependencies (except for system headers).
// It removes code unreachable from main function.
//
// In the following, it is important to distinguish 'semantic' and 'lexical' declarations.
//
// A semantic declaration is what a user (programmer) thinks of: *the* function f(), *the* class A.
// Note that different instantiations (implicit or explicit) of the same template are different
// semantic declarations.
//
// A lexical declaration is a node in the AST (represented by clang::Decl class) coming from a
// specific place in source code. Because of implicit code and template instantiations, multiple
// declarations may be generated by the same place in the source code.
//
// A semantic declaration may have multiple corresponding lexical declarations. For example,
// a class may have multiple forward declarations and one definition. We represent a semantic
// declarations by singling out one corresponding lexical declaration, given by
// Decl::getCanonicalDecl() method.
//
// Implementation is roughly as follows:
//
// 1. Build dependency graph for semantic declarations (defined either in main file or in system
//    headers).
// 2. Find semantic declarations that are reachable from main function in the graph.
// 3. Remove unnecessary lexical declarations from main file. If a semantic declaration is unused,
//    all corresponding lexical declarations may be removed. Otherwise, a deeper analysis, depending
//    on the type of the declaration, is required. For example, a forward declaration of a used class
//    might be removed.
// 4. Remove inactive preprocessor branches that have not yet been removed.
// 5. Remove preprocessor definitions, all usages of which are inside removed code.
//
//



class BuildNonImplicitDeclMap: public clang::RecursiveASTVisitor<BuildNonImplicitDeclMap> {
public:
    BuildNonImplicitDeclMap(SourceInfo& srcInfo_)
        : srcInfo(srcInfo_)
        , t("BuildNonImplicitDeclMap::VisitDecl")
    {
        t.pause();
    }

    bool shouldVisitImplicitCode() const { return false; }
    bool shouldVisitTemplateInstantiations() const { return false; }
    bool shouldWalkTypesOfTypeLocs() const { return false; }

    bool VisitDecl(clang::Decl* decl) {
        t.resume();
        auto key = SourceInfo::makeKey(decl);
        srcInfo.nonImplicitDecls.emplace(std::move(key), decl);
        t.pause();
        return true;
    }

private:
    SourceInfo& srcInfo;
    ScopedTimer t;
};

class OptimizerConsumer: public ASTConsumer {
public:
    OptimizerConsumer(CompilerInstance& compiler_,
            std::unique_ptr<SmartRewriter> smartRewriter_,
            RemoveInactivePreprocessorBlocks& ppCallbacks_,
            const std::unordered_set<string>& identifiersToKeep_,
            string& result_)
        : compiler(compiler_)
        , sourceManager(compiler.getSourceManager())
        , smartRewriter(std::move(smartRewriter_))
        , ppCallbacks(ppCallbacks_)
        , identifiersToKeep(identifiersToKeep_)
        , result(result_)
    {
    }

    virtual void HandleTranslationUnit(ASTContext& Ctx) override {
        // 0. Collect auxiliary information.
        {
            ScopedTimer t("BuildNonImplicitDeclMap");
            BuildNonImplicitDeclMap visitor(srcInfo);
            visitor.TraverseDecl(Ctx.getTranslationUnitDecl());
        }

        // 1. Build dependency graph for semantic declarations.
        {
            ScopedTimer t("DependenciesCollector");
            clang::Sema& sema = compiler.getSema();
            DependenciesCollector depsVisitor(sourceManager, sema, identifiersToKeep, srcInfo);
            depsVisitor.TraverseDecl(Ctx.getTranslationUnitDecl());

            // Source range of delayed-parsed template functions includes only declaration part.
            //     Force their parsing to get correct source ranges.
            //     Suppress error messages temporarily (it's OK for these functions
            //     to be malformed).
            DiagnosticsEngine& diag = sema.getDiagnostics();
            const bool suppressAll = diag.getSuppressAllDiagnostics();
            diag.setSuppressAllDiagnostics(true);
            for (FunctionDecl* f : srcInfo.delayedParsedFunctions) {
                auto& /*ptr to clang::LateParsedTemplate*/ lpt = sema.LateParsedTemplateMap[f];
                sema.LateTemplateParser(sema.OpaqueParser, *lpt);
            }
            diag.setSuppressAllDiagnostics(suppressAll);

#ifdef CAIDE_DEBUG_MODE
            std::ofstream file("caide-graph.dot");
            depsVisitor.printGraph(file);
#endif
        }

        // 2. Find semantic declarations that are reachable from main function in the graph.
        std::unordered_set<Decl*> used;
        {
            ScopedTimer t("BFS");
            set<Decl*> queue;
            for (Decl* decl : srcInfo.declsToKeep)
                queue.insert(decl->getCanonicalDecl());

            while (!queue.empty()) {
                Decl* decl = *queue.begin();
                queue.erase(queue.begin());
                if (used.insert(decl).second)
                    queue.insert(srcInfo.uses[decl].begin(), srcInfo.uses[decl].end());
            }
        }

        // 3. Remove unnecessary lexical declarations.
        std::unordered_set<Decl*> removedDecls;
        {
            ScopedTimer t("OptimizerVisitor");
            OptimizerVisitor visitor(sourceManager, used, removedDecls, *smartRewriter);
            visitor.TraverseDecl(Ctx.getTranslationUnitDecl());
            visitor.Finalize(Ctx);
        }
        {
            ScopedTimer t("MergeNamespacesVisitor");
            MergeNamespacesVisitor visitor(sourceManager, removedDecls, *smartRewriter);
            visitor.TraverseDecl(Ctx.getTranslationUnitDecl());
        }

        // 4. Remove inactive preprocessor branches that have not yet been removed.
        // 5. Remove preprocessor definitions, all usages of which are inside removed code.
        //
        // Callbacks have been called implicitly before this method, so we only need to call
        // Finalize() method that will actually use the information collected by callbacks
        // to remove unused preprocessor code
        ScopedTimer t("Finalize+Rewrite");
        ppCallbacks.Finalize();

        smartRewriter->applyChanges();

        result = getResult();
    }

private:
    string getResult() const {
        if (const RewriteBuffer* rewriteBuf =
                smartRewriter->getRewriteBufferFor(sourceManager.getMainFileID()))
            return string(rewriteBuf->begin(), rewriteBuf->end());

        // No changes
        bool invalid;
#if CAIDE_CLANG_VERSION_AT_LEAST(12, 0)
        const llvm::StringRef bufferData = sourceManager.getBufferData(sourceManager.getMainFileID(), &invalid);
        if (invalid)
            return "Inliner error";
        return string(bufferData.begin(), bufferData.end());
#else
        const llvm::MemoryBuffer* buf = sourceManager.getBuffer(sourceManager.getMainFileID(), &invalid);
        if (invalid)
            return "Inliner error";
        return string(buf->getBufferStart(), buf->getBufferEnd());
#endif
    }

private:
    CompilerInstance& compiler;
    SourceManager& sourceManager;
    std::unique_ptr<SmartRewriter> smartRewriter;
    RemoveInactivePreprocessorBlocks& ppCallbacks;
    const std::unordered_set<string>& identifiersToKeep;
    string& result;
    SourceInfo srcInfo;
};


class OptimizerFrontendAction : public ASTFrontendAction {
private:
    string& result;
    const set<string>& macrosToKeep;
    const std::unordered_set<string>& identifiersToKeep;
public:
    OptimizerFrontendAction(string& result_, const std::set<string>& macrosToKeep_,
            const std::unordered_set<string>& identifiersToKeep_)
        : result(result_)
        , macrosToKeep(macrosToKeep_)
        , identifiersToKeep(identifiersToKeep_)
    {}

    virtual std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance& compiler, StringRef /*file*/) override
    {
        if (!compiler.hasSourceManager())
            throw "No source manager";
        auto smartRewriter = std::unique_ptr<SmartRewriter>(
            new SmartRewriter(compiler.getSourceManager(), compiler.getLangOpts()));
        auto ppCallbacks = std::unique_ptr<RemoveInactivePreprocessorBlocks>(
            new RemoveInactivePreprocessorBlocks(compiler.getSourceManager(), compiler.getLangOpts(),
                *smartRewriter, macrosToKeep));
        auto consumer = std::unique_ptr<OptimizerConsumer>(
            new OptimizerConsumer(compiler, std::move(smartRewriter), *ppCallbacks, identifiersToKeep, result));
        compiler.getPreprocessor().addPPCallbacks(std::move(ppCallbacks));
        return consumer;
    }
};

class OptimizerFrontendActionFactory: public tooling::FrontendActionFactory {
private:
    string& result;
    const std::set<string>& macrosToKeep;
    const std::unordered_set<string>& identifiersToKeep;
public:
    OptimizerFrontendActionFactory(string& result_, const std::set<string>& macrosToKeep_,
            const std::unordered_set<string>& identifiersToKeep_)
        : result(result_)
        , macrosToKeep(macrosToKeep_)
        , identifiersToKeep(identifiersToKeep_)
    {}
#if CAIDE_CLANG_VERSION_AT_LEAST(10, 0)
    std::unique_ptr<FrontendAction> create() override {
        return std::make_unique<OptimizerFrontendAction>(result, macrosToKeep, identifiersToKeep);
    }
#else
    FrontendAction* create() override {
        return new OptimizerFrontendAction(result, macrosToKeep, identifiersToKeep);
    }
#endif
};

// Like clang::TextDiagnosticBuffer, but resolves source locations eagerly and
// adds them to error messages.
class ErrorCollector: public DiagnosticConsumer {
public:
    void HandleDiagnostic(DiagnosticsEngine::Level DiagLevel, const Diagnostic& Info) override {
        // Default implementation (Warnings/errors count).
        DiagnosticConsumer::HandleDiagnostic(DiagLevel, Info);
        if (DiagLevel >= DiagnosticsEngine::Level::Error) {
            string message;
            if (Info.hasSourceManager()) {
                message = Info.getLocation().printToString(Info.getSourceManager());
                message += ": ";
            }
            llvm::SmallVector<char, 256> buffer;
            Info.FormatDiagnostic(buffer);
            message += string(buffer.data(), buffer.size());
            errors.push_back(std::move(message));
        }
    }

    void clear() override {
        DiagnosticConsumer::clear();
        errors.clear();
    }

    const vector<string>& getErrors() const { return errors; }

private:
    vector<string> errors;
};

Optimizer::Optimizer(const vector<string>& cmdLineOptions_,
                     const vector<string>& macrosToKeep_,
                     const std::vector<std::string>& identifiersToKeep_)
    : cmdLineOptions(cmdLineOptions_)
    , macrosToKeep(macrosToKeep_.begin(), macrosToKeep_.end())
    , identifiersToKeep(identifiersToKeep_.begin(), identifiersToKeep_.end())
{}

string Optimizer::doOptimize(const string& cppFile) {
    ScopedTimer t("Optimizer::doOptimize");
    std::unique_ptr<tooling::FixedCompilationDatabase> compilationDatabase(
        createCompilationDatabaseFromCommandLine(cmdLineOptions));

    vector<string> sources;
    sources.push_back(cppFile);

    ErrorCollector errors;
    clang::tooling::ClangTool tool(*compilationDatabase, sources);
    tool.setDiagnosticConsumer(&errors);

    string result;
    OptimizerFrontendActionFactory factory(result, macrosToKeep, identifiersToKeep);

    ScopedTimer t2("Optimizer::tool.run");
    int ret = tool.run(&factory);
    if (ret != 0) {
        string message = "Inliner failed.";
        if (!errors.getErrors().empty()) {
            message += " The following compilation errors were detected: ";
            for (const auto& error : errors.getErrors()) {
                message += error;
                message.push_back('\n');
            }
        }
        throw std::runtime_error(message.c_str());
    }

    return result;
}

}
}

