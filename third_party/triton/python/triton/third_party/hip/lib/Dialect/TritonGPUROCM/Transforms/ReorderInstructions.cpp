#include "mlir/Analysis/SliceAnalysis.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/Dominance.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Interfaces/InferTypeOpInterface.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Support/LogicalResult.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "mlir/Transforms/Passes.h"
#include "mlir/Transforms/RegionUtils.h"
#include "triton/AnalysisROCM/Utility.h"
#include "triton/Dialect/TritonGPUROCM/IR/Dialect.h"
#include "triton/Dialect/TritonGPUROCM/Transforms/Passes.h"
#include "triton/Dialect/TritonGPUROCM/Transforms/TritonGPUConversion.h"
#include "triton/Dialect/TritonGPUROCM/Transforms/Utility.h"
#define GEN_PASS_CLASSES
#include "triton/Dialect/TritonGPUROCM/Transforms/Passes.h.inc"

using namespace mlir;

static inline bool
willIncreaseRegisterPressure(triton::gpu_rocm::ConvertLayoutOp op) {
  auto srcType = op.getOperand().getType().cast<RankedTensorType>();
  auto dstType = op.getResult().getType().cast<RankedTensorType>();
  auto srcEncoding = srcType.getEncoding();
  auto dstEncoding = dstType.getEncoding();
  if (srcEncoding.isa<triton::gpu_rocm::SharedEncodingAttr>())
    return true;
  if (dstEncoding.isa<triton::gpu_rocm::DotOperandEncodingAttr>())
    return true;
  return false;
}

class TritonGPUReorderInstructionsPass
    : public TritonGPUReorderInstructionsBase<
          TritonGPUReorderInstructionsPass> {
public:
  TritonGPUReorderInstructionsPass() = default;

  void runOnOperation() override {
    ModuleOp m = getOperation();
    mlir::DominanceInfo dom(m);
    // Sink conversions into loops when they will increase
    // register pressure
    DenseMap<Operation *, Operation *> opToMove;
    auto moveAfter = [](Operation *lhs, Operation *rhs) {
      auto lhsId = getWSRoleId(lhs);
      auto rhsId = getWSRoleId(rhs);
      if (lhsId == rhsId)
        lhs->moveAfter(rhs);
    };
    m.walk([&](triton::gpu_rocm::ConvertLayoutOp op) {
      if (!willIncreaseRegisterPressure(op))
        return;
      auto user_begin = op->user_begin();
      auto user_end = op->user_end();
      if (std::distance(user_begin, user_end) != 1)
        return;
      if (user_begin->getParentOfType<scf::ForOp>() ==
          op->getParentOfType<scf::ForOp>())
        return;
      opToMove.insert({op, *user_begin});
    });
    for (auto &kv : opToMove)
      kv.first->moveBefore(kv.second);
    // Move convert(load) immediately after dependent load
    m.walk([&](triton::gpu_rocm::ConvertLayoutOp op) {
      auto dstType = op.getResult().getType().cast<RankedTensorType>();
      auto dstEncoding = dstType.getEncoding();
      // Enable moving shared->dot conversion after dependent load.
      // For the Q tensor in flash attention, the shared->dot conversion acts as
      // a loop invariant. Moving this conversion post dependent load,
      // will hoist the conversion outside the loop. Consequently, during
      // computation, we will be able to maintain the Q tensor in the registers.
#if 1
      if (!dstEncoding.isa<triton::gpu_rocm::SharedEncodingAttr>() &&
          !dstEncoding.isa<triton::gpu_rocm::DotOperandEncodingAttr>())
        return;
#elif
      if (!dstEncoding.isa<triton::gpu_rocm::SharedEncodingAttr>())
        return;
#endif
      Operation *argOp = op.getOperand().getDefiningOp();
      if (!argOp)
        return;
      moveAfter(op, argOp);
    });
    // Move transpositions just after their definition
    opToMove.clear();
    m.walk([&](triton::TransOp op) {
      Operation *argOp = op.getOperand().getDefiningOp();
      if (!argOp)
        return;
      moveAfter(op, argOp);
    });
    // Move `dot` operand so that conversions to opIdx=1 happens after
    // conversions to opIdx=0
#if 1
    // Skip this reordering for ROCm backend since it will sink shared->dot
    // conversion for Q tensor in flash attention into the main loop. This
    // increases LDS pressure and requires additional computation in every loop
    // iteration.
    return;
#endif
    m.walk([&](triton::gpu_rocm::ConvertLayoutOp op) {
      auto dstType = op.getResult().getType().cast<RankedTensorType>();
      auto dstEncoding =
          dstType.getEncoding().dyn_cast<triton::gpu_rocm::DotOperandEncodingAttr>();
      if (!dstEncoding)
        return;
      int opIdx = dstEncoding.getOpIdx();
      if (opIdx != 1)
        return;
      if (op->getUsers().empty())
        return;
      auto dotUser = dyn_cast<triton::DotOp>(*op->user_begin());
      if (!dotUser)
        return;
      auto AOp =
          dotUser.getOperand(0).getDefiningOp<triton::gpu_rocm::ConvertLayoutOp>();
      if (!AOp)
        return;
      // Check that the conversion to OpIdx=1 happens before and can be moved
      // after the conversion to OpIdx=0.
      if (!dom.dominates(op.getOperation(), AOp.getOperation()))
        return;
      moveAfter(op, AOp);
    });
    return;
  }
};

std::unique_ptr<Pass> mlir::createTritonGPUROCMReorderInstructionsPass() {
  return std::make_unique<TritonGPUReorderInstructionsPass>();
}
