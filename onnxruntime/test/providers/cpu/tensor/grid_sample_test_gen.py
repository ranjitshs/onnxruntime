# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.

# This code is used to generate the test cases for the GridSample operator
# in onnxruntime/test/providers/cpu/tensor/grid_sample_test.cc

import torch

# Define the input dimensions
N, C, D, H, W = 2, 2, 3, 3, 2

# Define the modes, padding modes, and whether to align corners
modes = ["nearest", "bilinear", "bicubic"]
padding_modes = ["zeros", "border", "reflection"]
align_corners_options = [True, False]

print(
    """
template <typename T>
class GridSampleTest : public ::testing::Test {
};

using GridSampleTestTypes = ::testing::Types<float, MLFloat16>;
TYPED_TEST_SUITE(GridSampleTest, GridSampleTestTypes);

"""
)
# Loop over the combinations of parameters
torch.manual_seed(0)
for opset_version in [16, 20]:
    for mode in modes:
        for padding_mode in padding_modes:
            for align_corners in align_corners_options:
                for ndim in [4, 5]:
                    if ndim == 5 and mode == "bicubic":
                        continue

                    if opset_version < 20 and ndim == 5:
                        continue

                    # Create a random input tensor with the specified dimensions
                    input_shape = (N,) + (C,) + (((D, H, W)) if ndim == 5 else ((H, W)))
                    input_tensor = torch.randn(*input_shape)

                    # Create a random grid tensor with the specified dimensions
                    grid_shape = (N,) + (((D, H, W)) if ndim == 5 else ((H, W))) + (ndim - 2,)

                    # Between -1.2 to + 1.2
                    grid_tensor = torch.rand(*grid_shape) * 2.4 - 1.2

                    # Apply grid_sample
                    output_tensor = torch.nn.functional.grid_sample(
                        input_tensor, grid_tensor, mode=mode, padding_mode=padding_mode, align_corners=align_corners
                    )

                    X_data_str = "{" + ", ".join([f"TypeParam({x:.6f}f)" for x in input_tensor.numpy().flatten()]) + "}"
                    Grid_data_str = (
                        "{" + ", ".join([f"TypeParam({x:.6f}f)" for x in grid_tensor.numpy().flatten()]) + "}"
                    )

                    Y_shape = output_tensor.shape
                    Y_data_str = (
                        "{" + ", ".join([f"TypeParam({x:.6f}f)" for x in output_tensor.numpy().flatten()]) + "}"
                    )

                    onnx_mode = mode
                    if opset_version >= 20:
                        if mode == "bilinear":
                            onnx_mode = "linear"
                        elif mode == "bicubic":
                            onnx_mode = "cubic"

                    onnx_align_corners = 1 if align_corners else 0

                    test_name = f"test_grid_sample_{opset_version}_{ndim}D_{mode}_{padding_mode}_{'align_corners' if align_corners else 'no_align_corners'}"
                    spaces = "    "
                    print(f"TYPED_TEST(GridSampleTest, {test_name}) {{")
                    print(f'{spaces}OpTester test("GridSample", {opset_version});')
                    print(f'{spaces}std::string mode = "{onnx_mode}";')
                    print(f'{spaces}std::string padding_mode = "{padding_mode}";')
                    print(f"{spaces}int64_t align_corners = {onnx_align_corners};")
                    print(f"{spaces}std::initializer_list<int64_t> X_shape {{ {', '.join(map(str, input_shape))} }};")
                    print(f"{spaces}std::initializer_list<TypeParam> X_data {X_data_str};")
                    print(f"{spaces}std::initializer_list<int64_t> Grid_shape {{ {', '.join(map(str, grid_shape))} }};")
                    print(f"{spaces}std::initializer_list<TypeParam> Grid_data {Grid_data_str};")
                    print(f"{spaces}std::initializer_list<int64_t> Y_shape {{ {', '.join(map(str, Y_shape))} }};")
                    print(f"{spaces}std::initializer_list<TypeParam> Y_data {Y_data_str};")

                    print(f'{spaces}test.AddInput<TypeParam>("X", X_shape, X_data);')
                    print(f'{spaces}test.AddInput<TypeParam>("Grid", Grid_shape, Grid_data);')
                    print(f'{spaces}test.AddAttribute("mode", mode);')
                    print(f'{spaces}test.AddAttribute("padding_mode", padding_mode);')
                    print(f'{spaces}test.AddAttribute("align_corners", align_corners);')
                    print(f'{spaces}test.AddOutput<TypeParam>("Y", Y_shape, Y_data);')
                    print(f"{spaces}RunTests(test, GetExecutionProviders({opset_version}));")
                    print("}")
                    print("\n")
