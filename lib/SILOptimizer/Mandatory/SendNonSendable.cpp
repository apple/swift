#include "../../Sema/TypeCheckConcurrency.h"
#include "swift/AST/Expr.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"

using namespace swift;

class SendNonSendable : public SILFunctionTransform {

  // find any ApplyExprs in this function, and check if any of them make an
  // unsatisfied isolation jump, emitting appropriate diagnostics if so
  void run() override {
    SILFunction *function = getFunction();

    if (!function->getASTContext().LangOpts
        .hasFeature(Feature::DeferredSendableChecking))
      return;

    DeclContext *declContext = function->getDeclContext();

    for (SILBasicBlock &bb : *function) {
      for (SILInstruction &instr : bb) {
        if (ApplyExpr *apply = instr.getLoc().getAsASTNode<ApplyExpr>()) {
          diagnoseApplyArgSendability(apply, declContext);
        }
      }
    }
  }
};

/// This pass is known to depend on the following passes having run before it:
/// none so far
SILTransform *swift::createSendNonSendable() {
  return new SendNonSendable();
}
