/*
 * Copyright (c) 2023, Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#ifdef USE_ROCM

#include "../DotOpToLLVM.h"
#include "../Utility.h"
#include "triton/Dialect/TritonGPU/Transforms/Utility.h"

#include "mlir/Dialect/LLVMIR/ROCDLDialect.h"

using namespace mlir;
using namespace mlir::triton;

namespace {

using ::mlir::triton::gpu::DotOperandEncodingAttr;
using ::mlir::triton::gpu::MfmaEncodingAttr;
using ::mlir::triton::gpu::SharedEncodingAttr;

using ValueTable = std::map<std::pair<unsigned, unsigned>, Value>;

struct DotOpMFMAConversionHelper {
  MfmaEncodingAttr mfmaLayout;

  ConversionPatternRewriter &rewriter;
  TritonGPUToLLVMTypeConverter *typeConverter;
  Location loc;
  MLIRContext *ctx{};

  explicit DotOpMFMAConversionHelper(
      MfmaEncodingAttr mfmaLayout, ConversionPatternRewriter &rewriter,
      TritonGPUToLLVMTypeConverter *typeConverter, Location loc)
      : mfmaLayout(mfmaLayout), rewriter(rewriter),
        typeConverter(typeConverter), loc(loc), ctx(mfmaLayout.getContext()) {}

  Value getThreadId() const {
    auto llvmIndexTy = typeConverter->getIndexType();
    auto tid = rewriter.create<::mlir::gpu::ThreadIdOp>(
        loc, rewriter.getIndexType(), ::mlir::gpu::Dimension::x);
    return rewriter.create<arith::TruncIOp>(loc, i32_ty, tid);
  }

  Value generateMFMAOp(StringRef mfmaInsnName, Value valA, Value valB,
                       Value valC) const {
    auto resType = valC.getType();
    Value zeroFlag = i32_val(0);
    OperationState loweredOp(loc, mfmaInsnName);
    loweredOp.addTypes(resType);
    loweredOp.addOperands({valA, valB, valC, zeroFlag, zeroFlag, zeroFlag});
    return rewriter.create(loweredOp)->getResult(0);
  }

  int getNumSubmatrices(Type elementType, int mDim, int nDim) const {
    if (mDim == 64 && nDim == 4 || mDim == 4 && nDim == 64)
      return 1;
    assert(mDim == nDim);
    switch (mDim) {
    case 32:
    case 16:
      return 1;
      break;
    case 4:
      assert(elementType.getIntOrFloatBitWidth() <= 32 &&
             "fp64 is not supported yet");
      assert(elementType.getIntOrFloatBitWidth() != 8 ||
             elementType.isInteger(8) && "fp8 is not supported yet");
      return 16;
      break;
    default:
      llvm::report_fatal_error("unsupported nonKDim in MFMA dot");
    }
    return -1;
  }

  Value processSubBlocks(int numSubBlocks, Value acc, bool reduceSubBlocks,
                         bool zeroSubBlocks) const {
    assert((numSubBlocks & (numSubBlocks - 1)) == 0 &&
           "numSubBlocks in not pow 2!");
    if (numSubBlocks == 1)
      return acc;
    constexpr int waveSize = 64;
    int subBlockSize = waveSize / numSubBlocks;
    Value laneId = getThreadId();
    laneId = and_(laneId, i32_val(waveSize - 1));
    auto vecTy = dyn_cast<VectorType>(acc.getType());
    auto elemType = vecTy.getElementType();
    assert(elemType.getIntOrFloatBitWidth() == 32);
    int numScalars = vecTy.getNumElements();
    std::vector<Value> accScalar(numScalars);
    for (int i = 0; i < numScalars; ++i)
      accScalar[i] = extract_element(elemType, acc, i32_val(i));

    if (reduceSubBlocks) {
      while (subBlockSize < waveSize) {
        for (int i = 0; i < numScalars; ++i) {
          Value other_acc =
              mlir::LLVM::shflSync(loc, rewriter, accScalar[i], subBlockSize);
          if (elemType.isInteger(32))
            accScalar[i] = add(accScalar[i], other_acc);
          else
            accScalar[i] = fadd(accScalar[i], other_acc);
        }
        subBlockSize *= 2;
      }
    }
    if (zeroSubBlocks) {
      Value zero;
      if (elemType.isInteger(32))
        zero = i32_val(0);
      else
        zero = f32_val(0.0);
      auto cond = icmp_ult(laneId, i32_val(subBlockSize));
      for (int i = 0; i < numScalars; ++i)
        accScalar[i] = select(cond, accScalar[i], zero);
    }

    Value reducedAcc = undef(vecTy);
    for (int i = 0; i < numScalars; ++i)
      reducedAcc = insert_element(vecTy, reducedAcc, accScalar[i], i32_val(i));
    return reducedAcc;
  }

  /// @brief MFMA 4x4 is computes 16 matrix mupliplications, this functions adds
  /// these 16 matrices to get final 4x4 matrix
  /// @param numSubBlocks
  /// @param acc
  /// @return
  Value reduceSubBlocks(int numSubBlocks, Value acc) const {
    return processSubBlocks(numSubBlocks, acc, true, false);
  }

  /// @brief Zeroes out redundant values in all sub-blocks except first one
  ///
  /// Every wave in mfma 4x4 layout holds only 4 unique values(scalar or
  /// vectors) in blocks of 4 consecutive threads, There are 16 copies of these
  /// 4 values across all threads of the wave. Need to zero out 15 copies to use
  /// accumulator between dot operations.
  /// @param numSubBlocks
  /// @param acc
  /// @return
  Value zeroAuxiliarBlocks(int numSubBlocks, Value acc) const {
    return processSubBlocks(numSubBlocks, acc, false, true);
  }

  // Conduct the Dot conversion.
  LogicalResult convertDot(DotOp op, DotOpAdaptor adaptor) const {
    auto warpsPerCTA = mfmaLayout.getWarpsPerCTA();
    auto mDim = mfmaLayout.getMDim();
    auto nDim = mfmaLayout.getNDim();
    auto mfmaVersion = mfmaLayout.getVersionMajor();
    assert((mDim == nDim && (mDim == 32 || mDim == 16 || mDim == 4)) ||
           (mDim == 64 && nDim == 4) || (mDim == 4 && nDim == 64));

    Value a = op.getA();
    Value b = op.getB();
    Value d = op.getD();
    auto aTensorTy = a.getType().cast<RankedTensorType>();
    auto bTensorTy = b.getType().cast<RankedTensorType>();
    auto dTensorTy = d.getType().cast<RankedTensorType>();
    auto elemTyA = aTensorTy.getElementType();
    auto elemTyB = bTensorTy.getElementType();

    StringRef mfmaInsnName;
    auto maybeMfmaInsn =
        MfmaInsn::selectMfma(mDim, nDim, elemTyA, elemTyB, mfmaVersion);
    if (failed(maybeMfmaInsn))
      llvm::report_fatal_error("No match found in MFMA database\n");

    mfmaInsnName = (*maybeMfmaInsn).getInsnName();
    unsigned k_base = (*maybeMfmaInsn).getKBase();

    auto aEncoding = aTensorTy.getEncoding().cast<DotOperandEncodingAttr>();
    auto bEncoding = bTensorTy.getEncoding().cast<DotOperandEncodingAttr>();

    auto kWidth = aEncoding.getKWidth();
    assert(kWidth == bEncoding.getKWidth());

    auto repA = aEncoding.getMFMARep(aTensorTy.getShape());
    auto repB = bEncoding.getMFMARep(bTensorTy.getShape());

    assert(repA[1] == repB[0]);

    Value loadedA = adaptor.getA();
    Value loadedB = adaptor.getB();
    Value loadedC = adaptor.getC();

    auto numRepM = repA[0];
    auto numRepN = repB[1];
    auto numRepK = repA[1];

    auto operandA = getValuesFromDotOperandLayoutStruct(
        loadedA, numRepM, numRepK, kWidth, k_base, aTensorTy.getElementType());
    auto operandB = getValuesFromDotOperandLayoutStruct(
        loadedB, numRepN, numRepK, kWidth, k_base, aTensorTy.getElementType());

    auto dstElemTy = dTensorTy.getElementType();
    auto fc =
        typeConverter->unpackLLElements(loc, loadedC, rewriter, dstElemTy);

    unsigned warpSize = triton::gpu::getWarpSize(mfmaLayout);
    // compute number of output elements that each thread holds for one MFMA
    // instruction. subBlocks
    const int subBlocks =
        getNumSubmatrices(aTensorTy.getElementType(), mDim, nDim);
    auto elemsPerVec = mDim * nDim * subBlocks / warpSize;

    auto vecTy = vec_ty(dstElemTy, elemsPerVec);
    for (int m = 0; m < numRepM; ++m) {
      for (int n = 0; n < numRepN; ++n) {
        Value acc = undef(vecTy);
        for (unsigned v = 0; v < elemsPerVec; ++v) {
          acc = insert_element(
              vecTy, acc, fc[m * numRepN * elemsPerVec + n * elemsPerVec + v],
              i32_val(v));
        }

        acc = zeroAuxiliarBlocks(subBlocks, acc);
        for (size_t k = 0; k < numRepK; k++)
          for (int kpack = 0; kpack < kWidth / k_base; ++kpack)
            acc = mfmaLayout.getIsTransposed()
                      ? generateMFMAOp(mfmaInsnName, operandB[kpack][{n, k}],
                                       operandA[kpack][{m, k}], acc)
                      : generateMFMAOp(mfmaInsnName, operandA[kpack][{m, k}],
                                       operandB[kpack][{n, k}], acc);
        acc = reduceSubBlocks(subBlocks, acc);
        for (unsigned v = 0; v < elemsPerVec; ++v) {
          fc[m * numRepN * elemsPerVec + n * elemsPerVec + v] =
              extract_element(dstElemTy, acc, i32_val(v));
        }
      }
    }

    // replace with new packed result
    Type structTy = LLVM::LLVMStructType::getLiteral(
        ctx, SmallVector<Type>(fc.size(), dstElemTy));
    Value res = typeConverter->packLLElements(loc, fc, rewriter, structTy);
    rewriter.replaceOp(op, res);

    return success();
  }

  /**
   * @brief extract vector from rawElems based on kWidth and k_base
   * rawElems is a vector of kWidth elements. We need to prepare vector(s) of
   * k_base elements for each mfma instruction
   */
  SmallVector<Value> extractOperands(Value rawElems, int kWidth, int k_base,
                                     Type type) const {
    int kpack = kWidth / k_base;
    SmallVector<Value> results;
    auto vecTy = vec_ty(type, k_base);
    for (int k = 0; k < kpack; ++k) {
      Value vec = undef(vecTy);
      for (int elemId = 0; elemId < k_base; ++elemId) {
        auto val =
            extract_element(type, rawElems, i32_val(elemId + k * k_base));
        vec = insert_element(vecTy, vec, val, i32_val(elemId));
      }
      if (type.getIntOrFloatBitWidth() == 8) {
        if (4 == k_base)
          // This is for int8 on pre- MI300 GPUs
          results.push_back(bitcast(vec, i32_ty));
        if (8 == k_base)
          results.push_back(bitcast(vec, i64_ty));
      } else
        results.push_back(vec);
    }
    return results;
  }

  /**
   * @brief Converts dot operand structure to value table and converts types
   * appropriate for mfma instructions
   */
  SmallVector<ValueTable>
  getValuesFromDotOperandLayoutStruct(Value value, int n0, int n1, int kWidth,
                                      int k_base, Type type) const {
    auto elems = typeConverter->unpackLLElements(loc, value, rewriter, type);
    ValueTable vals;
    ValueTable vals1;
    int kpack = kWidth / k_base;
    SmallVector<ValueTable> dotOpVals(kpack);
    for (int i = 0; i < n0; i++) {
      for (int j = 0; j < n1; j++) {
        auto rawElems = elems[n1 * i + j];

        if (type.isF32()) {
          for (int k = 0; k < kpack; ++k) {
            dotOpVals[k][{i, j}] = extract_element(type, rawElems, i32_val(k));
          }
        } else {
          SmallVector<Value> vals;
          if (type.getIntOrFloatBitWidth() == 8) {
            vals = extractOperands(rawElems, kWidth, k_base, i8_ty);
          } else if (type.isBF16()) {
            vals = extractOperands(rawElems, kWidth, k_base, i16_ty);
          } else {
            assert(type.isF16() && "Unsupported data type");
            vals = extractOperands(rawElems, kWidth, k_base, f16_ty);
          }
          for (int k = 0; k < kpack; ++k) {
            dotOpVals[k][{i, j}] = vals[k];
          }
        }
      }
    }
    return dotOpVals;
  }
};

} // namespace

LogicalResult convertMFMA(triton::DotOp op, triton::DotOp::Adaptor adaptor,
                          TritonGPUToLLVMTypeConverter *typeConverter,
                          ConversionPatternRewriter &rewriter) {
  auto rankedTType = [](Value tensor) {
    return tensor.getType().cast<RankedTensorType>();
  };

  assert(rankedTType(op.getA()).getEncoding().isa<DotOperandEncodingAttr>() &&
         rankedTType(op.getB()).getEncoding().isa<DotOperandEncodingAttr>() &&
         "Both $a and %b should be DotOperand layout.");

  auto cTensorTy = rankedTType(op.getC());
  auto dTensorTy = rankedTType(op.getD());
  assert(cTensorTy.getEncoding().isa<MfmaEncodingAttr>() &&
         "Currently, we only support $c with a mfma layout.");

  assert(cTensorTy.getShape()[0] == dTensorTy.getShape()[0] &&
         cTensorTy.getShape()[1] == dTensorTy.getShape()[1] &&
         "DotOp's $c operand should pass the same number of values as $d");

  auto loc = op.getLoc();
  auto mfmaLayout = op.getResult()
                        .getType()
                        .cast<RankedTensorType>()
                        .getEncoding()
                        .cast<MfmaEncodingAttr>();

  DotOpMFMAConversionHelper helper(mfmaLayout, rewriter, typeConverter, loc);

  return helper.convertDot(op, adaptor);
}

#endif // ifdef USE_ROCM
