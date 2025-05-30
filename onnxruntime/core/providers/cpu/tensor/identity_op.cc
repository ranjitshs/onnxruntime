// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/providers/cpu/tensor/identity_op.h"

namespace onnxruntime {

ONNX_CPU_OPERATOR_VERSIONED_KERNEL(
    Dropout,
    7, 9,
    KernelDefBuilder().TypeConstraint("T", {DataTypeImpl::GetTensorType<MLFloat16>(),
                                            DataTypeImpl::GetTensorType<float>(),
                                            DataTypeImpl::GetTensorType<double>()}),
    IdentityOp<true>);

ONNX_CPU_OPERATOR_VERSIONED_KERNEL(
    Dropout,
    10,
    11,
    KernelDefBuilder().TypeConstraint("T", {DataTypeImpl::GetTensorType<MLFloat16>(),
                                            DataTypeImpl::GetTensorType<float>(),
                                            DataTypeImpl::GetTensorType<double>()})
        .TypeConstraint("T1", DataTypeImpl::GetTensorType<bool>()),
    IdentityOp<true>);

ONNX_CPU_OPERATOR_VERSIONED_KERNEL(
    Identity,
    1,
    12,
    KernelDefBuilder().TypeConstraint("T", DataTypeImpl::AllTensorTypes()).Alias(0, 0),
    IdentityOp<false>);

ONNX_CPU_OPERATOR_VERSIONED_KERNEL(
    Identity,
    13,
    13,
    KernelDefBuilder().TypeConstraint("T", DataTypeImpl::AllTensorTypes()).Alias(0, 0),
    IdentityOp<false>);

// Opset 14 supported sequence type
ONNX_CPU_OPERATOR_VERSIONED_KERNEL(
    Identity,
    14, 15,
    KernelDefBuilder().TypeConstraint("V", DataTypeImpl::AllTensorAndSequenceTensorTypes()).Alias(0, 0),
    IdentityOp<false>);

// Opset 16 supported optional type
ONNX_CPU_OPERATOR_VERSIONED_KERNEL(
    Identity,
    16, 18,
    KernelDefBuilder().TypeConstraint("V", DataTypeImpl::AllTensorAndSequenceTensorAndOptionalTypes()).Alias(0, 0),
    IdentityOp<false>);

ONNX_CPU_OPERATOR_VERSIONED_KERNEL(
    Identity,
    19, 20,
    KernelDefBuilder().TypeConstraint("V", DataTypeImpl::AllTensorAndSequenceTensorAndOptionalTypesIRv9()).Alias(0, 0),
    IdentityOp<false>);

// TODO(liqunfu): Opset 21 supported int4 and uint4 types.
ONNX_CPU_OPERATOR_VERSIONED_KERNEL(
    Identity,
    21,
    22,
    KernelDefBuilder().TypeConstraint("V", DataTypeImpl::AllTensorAndSequenceTensorAndOptionalTypesIRv9()).Alias(0, 0),
    IdentityOp<false>);

// Opset 23 added support for float4e2m1.
// TODO(titaiwang): Add support for float4e2m1.
ONNX_CPU_OPERATOR_KERNEL(
    Identity,
    23,
    KernelDefBuilder().TypeConstraint("V", DataTypeImpl::AllTensorAndSequenceTensorAndOptionalTypesIRv9()).Alias(0, 0),
    IdentityOp<false>);

}  // namespace onnxruntime
