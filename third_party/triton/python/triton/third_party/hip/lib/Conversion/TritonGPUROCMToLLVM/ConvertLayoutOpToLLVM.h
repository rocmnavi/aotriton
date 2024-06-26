#ifndef TRITON_CONVERSION_TRITONGPUROCM_TO_LLVM_CONVERT_LAYOUT_OP_H
#define TRITON_CONVERSION_TRITONGPUROCM_TO_LLVM_CONVERT_LAYOUT_OP_H

#include "TritonGPUToLLVMBase.h"

using namespace mlir;
using namespace mlir::triton;

using ::mlir::triton::gpu_rocm::DotOperandEncodingAttr;

void populateConvertLayoutOpToLLVMPatterns(
    TritonGPUToLLVMTypeConverter &typeConverter, RewritePatternSet &patterns,
    int numWarps, ModuleAxisInfoAnalysis &axisInfoAnalysis,
    ModuleAllocation &allocation,
    ConvertTritonGPUOpToLLVMPatternBase::IndexCacheInfo &indexCacheInfo,
    PatternBenefit benefit);

#endif
