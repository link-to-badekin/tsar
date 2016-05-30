//===--- tsar_action.cpp ---- TSAR Frontend Action --------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// This file implements front-end action which is necessary to analyze and
// transform sources. It also implements LLVM passes which initializes rewriter
// to transform sources in subsequent passes.
//
//===----------------------------------------------------------------------===//

#include <clang/AST/DeclCXX.h>
#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/CodeGen/ModuleBuilder.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendDiagnostic.h>
#include <clang/Sema/Sema.h>
#include <clang/Serialization/ASTReader.h>
#include <llvm/Analysis/Passes.h>
#include <llvm/CodeGen/Passes.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/IRPrintingPasses.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Pass.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Timer.h>
#include <memory>
#include "tsar_action.h"
#include "tsar_instrumentation.h"
#include "tsar_query.h"
#include "tsar_pass.h"
#include "tsar_transformation.h"

using namespace clang;
using namespace clang::tooling;
using namespace llvm;
using namespace tsar;

void QueryManager::run(llvm::Module *M, TransformationContext *Ctx) {
  assert(M && "Module must not be null!");
  legacy::PassManager Passes;
  if (Ctx) {
    auto TEP = static_cast<TransformationEnginePass *>(
      createTransformationEnginePass());
    TEP->setContext(*M, Ctx);
    Passes.add(TEP);
  }
  // The 'unreachableblockelim' pass is necessary because implementation
  // of data-flow analysis relies on suggestion that control-flow graph does
  // not contain unreachable basic blocks.
  // For the example int main() {exit(1);} 'clang' will generate the LLVM IR:
  // define i32 @main() #0 {
  // entry:
  //  %retval = alloca i32, align 4
  //  store i32 0, i32* %retval
  //   call void @exit(i32 1) #2, !dbg !11
  //  unreachable, !dbg !11
  // return:
  //  %0 = load i32, i32* %retval, !dbg !12
  //  ret i32 %0, !dbg !12
  //}
  // In other cases 'clang' automatically deletes unreachable blocks.
  if (Pass *P = createInitializationPass())
    Passes.add(P);
  Passes.add(createUnreachableBlockEliminationPass());
  Passes.add(createBasicAliasAnalysisPass());
  Passes.add(createPrivateRecognitionPass());
  if (Ctx)
    Passes.add(createPrivateCClassifierPass());
  Passes.add(createVerifierPass());
  if (Pass *P = createFinalizationPass())
    Passes.add(P);
  cl::PrintOptionValues();
  Passes.run(*M);
}

bool EmitLLVMQueryManager::beginSourceFile(
    CompilerInstance &CI, StringRef InFile) {
  mOS = CI.createDefaultOutputFile(false, InFile, "ll");
  mCodeGenOpts = &CI.getCodeGenOpts();
  return mOS && mCodeGenOpts;
}

void EmitLLVMQueryManager::run(llvm::Module *M, TransformationContext *) {
  assert(M && "Module must not be null!");
  legacy::PassManager Passes;
  if (Pass *P = createInitializationPass())
    Passes.add(P);
  Passes.add(createPrintModulePass(*mOS, "", mCodeGenOpts->EmitLLVMUseLists));
  if (Pass *P = createFinalizationPass())
    Passes.add(P);
  cl::PrintOptionValues();
  Passes.run(*M);
}

Pass *InstrLLVMQueryManager::createInitializationPass() {
  return createInstrumentationPass();
}

namespace clang {
/// This consumer builds LLVM IR for the specified file and launch analysis of
/// the LLVM IR.
class AnalysisConsumer : public ASTConsumer {
public:
  /// Constructor.
  AnalysisConsumer(AnalysisActionBase::Kind AK,
      CompilerInstance &CI, StringRef InFile,
      TransformationContext *TransformContext, ArrayRef<std::string> CL,
      QueryManager *QM)
    : mAction(AK), mLLVMIRGeneration("LLVM IR Generation Time"),
    mASTContext(nullptr), mLLVMContext(new LLVMContext),
    mGen(CreateLLVMCodeGen(CI.getDiagnostics(), InFile,
      CI.getHeaderSearchOpts(), CI.getPreprocessorOpts(),
      CI.getCodeGenOpts(), *mLLVMContext)),
    mTransformContext(TransformContext),
    mCommandLine(CL), mQueryManager(QM) {
    assert(AnalysisActionBase::FIRST_KIND <= AK &&
      AK <= AnalysisActionBase::LAST_KIND  && "Unknown kind of action!");
    assert((AK != AnalysisActionBase::KIND_TRANSFORM || mTransformContext) &&
      "For a configure rewriter action context must not be null!");
  }

  ~AnalysisConsumer() {
    // Be careful if kind of action KIND_TRANSFORM the transformation
    // context should not be deleted with this consumer.
    if (mAction == AnalysisActionBase::KIND_TRANSFORM)
      mTransformContext.release();
  }

  void HandleCXXStaticMemberVarInstantiation(VarDecl *VD) override {
    mGen->HandleCXXStaticMemberVarInstantiation(VD);
  }

  void Initialize(ASTContext &Ctx) override {
    if (mASTContext) {
      assert(mASTContext == &Ctx &&
        "Existed context must be equal with the specified!");
      return;
    }
    mASTContext = &Ctx;
    if (!mTransformContext)
      mTransformContext.reset(
        new TransformationContext(Ctx, *mGen, mCommandLine));
    else
      mTransformContext->reset(Ctx, *mGen);
    if (llvm::TimePassesIsEnabled)
      mLLVMIRGeneration.startTimer();
    mGen->Initialize(Ctx);
    mModule.reset(mGen->GetModule());
    if (llvm::TimePassesIsEnabled)
      mLLVMIRGeneration.stopTimer();
  }

  bool HandleTopLevelDecl(DeclGroupRef D) override {
    PrettyStackTraceDecl CrashInfo(*D.begin(), SourceLocation(),
      mASTContext->getSourceManager(), "LLVM IR generation of declaration");
    if (llvm::TimePassesIsEnabled)
      mLLVMIRGeneration.startTimer();
    mGen->HandleTopLevelDecl(D);
    if (llvm::TimePassesIsEnabled)
      mLLVMIRGeneration.stopTimer();
    return true;
  }

  void HandleInlineMethodDefinition(CXXMethodDecl *D) override {
    PrettyStackTraceDecl CrashInfo(D, SourceLocation(),
      mASTContext->getSourceManager(), "LLVM IR generation of inline method");
    if (llvm::TimePassesIsEnabled)
      mLLVMIRGeneration.startTimer();
    mGen->HandleInlineMethodDefinition(D);
    if (llvm::TimePassesIsEnabled)
      mLLVMIRGeneration.stopTimer();
  }

  void HandleTranslationUnit(ASTContext &ASTCtx) override {
    {
      PrettyStackTraceString CrashInfo("Per-file LLVM IR generation");
      if (llvm::TimePassesIsEnabled)
        mLLVMIRGeneration.startTimer();
      mGen->HandleTranslationUnit(ASTCtx);
      if (llvm::TimePassesIsEnabled)
        mLLVMIRGeneration.stopTimer();
    }
    // Silently ignore if we weren't initialized for some reason.
    if (!mModule)
      return;
    // Make sure IR generation is happy with the module. This is released by
    // the module provider.
    llvm::Module *M = mGen->ReleaseModule();
    if (!M) {
      // The module has been released by IR gen on failures, do not double
      // free.
      mModule.release();
      return;
    }
    assert(mModule.get() == M &&
      "Unexpected module change during IR generation");
    Timer LLVMIRAnalysis("LLVM IR Analysis Time");
    switch (mAction) {
      case AnalysisActionBase::KIND_ANALYSIS:
        if (llvm::TimePassesIsEnabled)
          LLVMIRAnalysis.startTimer();
        mQueryManager->run(M, mTransformContext.release());
        if (llvm::TimePassesIsEnabled)
          LLVMIRAnalysis.stopTimer();
        break;
      case AnalysisActionBase::KIND_TRANSFORM:
          llvm_unreachable("Transformation action is not implemented yet!");
          mTransformContext.release();
        break;
      default: assert("Unknown kind of action, so do nothing!"); break;
    }
  }

  void HandleTagDeclDefinition(TagDecl *D) override {
    PrettyStackTraceDecl CrashInfo(D, SourceLocation(),
      mASTContext->getSourceManager(), "LLVM IR generation of declaration");
    mGen->HandleTagDeclDefinition(D);
  }

  void HandleTagDeclRequiredDefinition(const TagDecl *D) override {
    mGen->HandleTagDeclRequiredDefinition(D);
  }

  void CompleteTentativeDefinition(VarDecl *D) override {
    mGen->CompleteTentativeDefinition(D);
  }

  void HandleVTable(CXXRecordDecl *RD) override {
    mGen->HandleVTable(RD);
  }

  void HandleLinkerOptionPragma(llvm::StringRef Opts) override {
    mGen->HandleLinkerOptionPragma(Opts);
  }

  void HandleDetectMismatch(
      llvm::StringRef Name, llvm::StringRef Value) override {
    mGen->HandleDetectMismatch(Name, Value);
  }

  void HandleDependentLibrary(llvm::StringRef Opts) override {
    mGen->HandleDependentLibrary(Opts);
  }

private:
  AnalysisActionBase::Kind mAction;
  Timer mLLVMIRGeneration;
  ASTContext *mASTContext;
  std::unique_ptr<llvm::LLVMContext> mLLVMContext;
  std::unique_ptr<CodeGenerator> mGen;
  std::unique_ptr<TransformationContext> mTransformContext;
  QueryManager *mQueryManager;
  ArrayRef<std::string> mCommandLine;
  std::unique_ptr<llvm::Module> mModule;
};
}

AnalysisActionBase::AnalysisActionBase(Kind AK, TransformationContext *Ctx,
    ArrayRef<std::string> CL, QueryManager *QM) :
  mKind(AK), mTransformContext(Ctx), mCommandLine(CL), mQueryManager(QM) {
  assert(FIRST_KIND <= AK && AK <= LAST_KIND && "Unknown kind of action!");
  assert((AK != KIND_TRANSFORM || Ctx) &&
    "For a transformation action context must not be null!");
  assert((QM || AK != KIND_ANALYSIS) &&
    "For analysis action query manager must be specified!");
  if (mTransformContext)
    mCommandLine = mTransformContext->getCommandLine();
}

bool AnalysisActionBase::BeginSourceFileAction(
    CompilerInstance &CI, StringRef InFile) {
  TimePassesIsEnabled = CI.getFrontendOpts().ShowTimers;
  return mQueryManager->beginSourceFile(CI, InFile);
}

void AnalysisActionBase::EndSourceFileAction() {
  mQueryManager->endSourceFile();
}

std::unique_ptr<ASTConsumer>
AnalysisActionBase::CreateASTConsumer(CompilerInstance &CI, StringRef InFile) {
  return std::unique_ptr<AnalysisConsumer>(
    new AnalysisConsumer(mKind, CI, InFile, mTransformContext, mCommandLine,
      mQueryManager));
}

void AnalysisActionBase::ExecuteAction() {
  // If this is an IR file, we have to treat it specially.
  if (getCurrentFileKind() != IK_LLVM_IR) {
    ASTFrontendAction::ExecuteAction();
    return;
  }
  if (mKind != AnalysisActionBase::KIND_ANALYSIS) {
    errs() << getCurrentFile() << " error: requested action is not available\n";
    return;
  }
  bool Invalid;
  CompilerInstance &CI = getCompilerInstance();
  SourceManager &SM = CI.getSourceManager();
  FileID FID = SM.getMainFileID();
  llvm::MemoryBuffer *MainFile = SM.getBuffer(FID, &Invalid);
  if (Invalid)
    return;
  llvm::SMDiagnostic Err;
  LLVMContext Ctx;
  std::unique_ptr<llvm::Module> M =
    parseIR(MainFile->getMemBufferRef(), Err, Ctx);
  if (!M) {
    // Translate from the diagnostic info to the SourceManager location if
    // available.
    SourceLocation Loc;
    if (Err.getLineNo() > 0) {
      assert(Err.getColumnNo() >= 0);
      Loc = SM.translateFileLineCol(SM.getFileEntryForID(FID),
        Err.getLineNo(), Err.getColumnNo() + 1);
    }
    // Strip off a leading diagnostic code if there is one.
    StringRef Msg = Err.getMessage();
    if (Msg.startswith("error: "))
      Msg = Msg.substr(7);
    unsigned DiagID =
      CI.getDiagnostics().getCustomDiagID(DiagnosticsEngine::Error, "%0");
    CI.getDiagnostics().Report(Loc, DiagID) << Msg;
    return;
  }
  const TargetOptions &TargetOpts = CI.getTargetOpts();
  if (M->getTargetTriple() != TargetOpts.Triple) {
    CI.getDiagnostics().Report(SourceLocation(),
      diag::warn_fe_override_module)
      << TargetOpts.Triple;
    M->setTargetTriple(TargetOpts.Triple);
  }
  Timer LLVMIRAnalysis("LLVM IR Analysis Time");
  if (llvm::TimePassesIsEnabled)
    LLVMIRAnalysis.startTimer();
  mQueryManager->run(M.get(), nullptr);
  if (llvm::TimePassesIsEnabled)
    LLVMIRAnalysis.stopTimer();
  return;
}

bool AnalysisActionBase::hasIRSupport() const { return true; }

MainAction::MainAction(ArrayRef<std::string> CL, QueryManager *QM) :
  AnalysisActionBase(KIND_ANALYSIS, nullptr, CL, QM) {}

TransformationAction::TransformationAction(TransformationContext &Ctx) :
  AnalysisActionBase(KIND_TRANSFORM, &Ctx,
    makeArrayRef<std::string>(""), nullptr) {}
