/* Copyright (c) 2019 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#include "paddle/fluid/operators/unsqueeze_op.h"

#include <memory>
#include <string>
#include <vector>

#include "paddle/fluid/framework/infershape_utils.h"
#include "paddle/fluid/framework/op_registry.h"
#include "paddle/phi/infermeta/unary.h"

namespace paddle {
namespace operators {

class UnsqueezeOp : public framework::OperatorWithKernel {
 public:
  using framework::OperatorWithKernel::OperatorWithKernel;

  void InferShape(framework::InferShapeContext *ctx) const override {
    PADDLE_ENFORCE_EQ(ctx->HasInput("X"),
                      true,
                      platform::errors::InvalidArgument(
                          "Input(X) of "
                          "Unsqueeze operator should not be null."));
    PADDLE_ENFORCE_EQ(ctx->HasOutput("Out"),
                      true,
                      platform::errors::InvalidArgument(
                          "Output(Out) of "
                          "Unsqueeze operator should not be null."));

    const auto &axes = ctx->Attrs().Get<std::vector<int>>("axes");
    const auto &x_dims = ctx->GetInputDim("X");
    // Validity Check: input tensor dims (<6).
    PADDLE_ENFORCE_LE(x_dims.size(),
                      6,
                      platform::errors::InvalidArgument(
                          "Invalid "
                          "dimensions, the rank of Input(X) "
                          "should be in the range of [1, 6] (Eigen limit)"));
    if (!axes.empty()) {
      auto out_dims = GetOutputShape(axes, x_dims);
      ctx->SetOutputDim("Out", out_dims);
      if (x_dims[0] == out_dims[0]) {
        // Only pass LoD when the first dimension of output and Input(X)
        // are the same.
        ctx->ShareLoD("X", "Out");
      }
    } else if (ctx->HasInputs("AxesTensorList")) {
      auto AxesTensorList = ctx->Inputs("AxesTensorList");
      int output_size = x_dims.size() + static_cast<int>(AxesTensorList.size());
      PADDLE_ENFORCE_LE(output_size,
                        6,
                        platform::errors::InvalidArgument(
                            "The output tensor's rank should be less than 6."));
      std::vector<int> vec_out_dims(output_size, -1);
      ctx->SetOutputDim("Out", phi::make_ddim(vec_out_dims));
    } else if (ctx->HasInput("AxesTensor")) {
      auto axes_dims = ctx->GetInputDim("AxesTensor");
      PADDLE_ENFORCE_EQ(axes_dims.size(),
                        1,
                        platform::errors::InvalidArgument(
                            "Input(AxesTensor)'s dimension of "
                            "Op(unsqueeze) must be 1. "
                            "But received AxesTensor's shape = [%s], "
                            "AxesTensor's dimension = %d.",
                            axes_dims,
                            axes_dims.size()));
      PADDLE_ENFORCE_GE(
          axes_dims[0],
          0,
          platform::errors::InvalidArgument(
              "Input(AxesTensor)'s shape must be known. But received "
              "AxesTensor's shape = [%s]",
              axes_dims));
      int output_size = x_dims.size() + static_cast<int>(axes_dims[0]);
      PADDLE_ENFORCE_LE(output_size,
                        6,
                        platform::errors::InvalidArgument(
                            "The output tensor's rank should be less than 6."));
      std::vector<int> vec_out_dims(output_size, -1);
      ctx->SetOutputDim("Out", phi::make_ddim(vec_out_dims));
    }
  }

  static framework::DDim GetOutputShape(const std::vector<int> unsqz_dims,
                                        const framework::DDim &in_dims) {
    int output_size = in_dims.size() + static_cast<int>(unsqz_dims.size());
    int cur_output_size = in_dims.size();
    std::vector<int64_t> output_shape(output_size, 0);

    // Validity Check: rank range.
    PADDLE_ENFORCE_LE(output_size,
                      6,
                      platform::errors::InvalidArgument(
                          "The output tensor's rank should be less than 6."));

    for (int axis : unsqz_dims) {
      int cur = axis < 0 ? axis + cur_output_size + 1 : axis;
      // Vaildity Check: the axis bound
      PADDLE_ENFORCE_GE(
          cur,
          0,
          platform::errors::InvalidArgument("The insert dimension value should "
                                            "not be less than 0"));
      PADDLE_ENFORCE_LE(cur,
                        cur_output_size,
                        platform::errors::InvalidArgument(
                            "The insert dimension value shoud not be larger "
                            "than the dimension size of input tensor"));
      // Move old axis, and insert new axis
      for (int i = cur_output_size; i >= cur; --i) {
        if (output_shape[i] == 1) {
          // Move axis
          output_shape[i + 1] = 1;
          output_shape[i] = 0;
        }
      }
      output_shape[cur] = 1;
      // Add the output size.
      cur_output_size++;
    }

    // Make output shape
    for (int in_idx = 0, out_idx = 0; out_idx < output_size; ++out_idx) {
      if (output_shape[out_idx] == 0) {
        output_shape[out_idx] = in_dims[in_idx++];
      }
    }

    return phi::make_ddim(output_shape);
  }

 protected:
  framework::OpKernelType GetExpectedKernelType(
      const framework::ExecutionContext &ctx) const override {
    return framework::OpKernelType(
        framework::TransToProtoVarType(
            ctx.Input<phi::DenseTensor>("X")->type()),
        ctx.device_context());
  }

  framework::OpKernelType GetKernelTypeForVar(
      const std::string &var_name,
      const phi::DenseTensor &tensor,
      const framework::OpKernelType &expected_kernel_type) const override {
    if (var_name == "AxesTensor" || var_name == "AxesTensorList") {
      return expected_kernel_type;
    }
    return framework::OpKernelType(
        expected_kernel_type.data_type_, tensor.place(), tensor.layout());
  }
};

class UnsqueezeOpMaker : public framework::OpProtoAndCheckerMaker {
 public:
  void Make() override {
    AddInput("X", "(Tensor). The input tensor of unsqueeze operator.");
    AddInput("AxesTensor",
             "(Tensor<int32>, optional). The dimensions to be inserted. "
             "If it exists, it will replace Attr(axes).")
        .AsDispensable();
    AddInput(
        "AxesTensorList",
        "(vector<Tensor<int32>>, optional). The dimensions to be inserted. "
        "If it exists, it will replace Attr(axes)."
        "The shape of the element in vector must be [1].")
        .AsDuplicable()
        .AsDispensable();
    AddOutput("Out", "(Tensor). The output tensor of unsqueeze operator.");
    AddAttr<std::vector<int>>("axes",
                              "(std::vector<int>). List of integers,"
                              " indicating the dimensions to be inserted")
        .SetDefault({})
        .AddCustomChecker([](const std::vector<int> &axes) {
          // Validity Check: axes dims (<6).
          PADDLE_ENFORCE_LT(static_cast<int>(axes.size()),
                            6,
                            platform::errors::InvalidArgument(
                                "Invalid "
                                "dimensions, dynamic dimensions should be "
                                "within [1, 6] dimensions (Eigen limit)."));
          // Validity Check: the range of unsqueeze axis.
          for (int axis : axes) {
            PADDLE_ENFORCE_LT(axis,
                              6,
                              platform::errors::InvalidArgument(
                                  "Invalid "
                                  "dimensions, input axis should be"
                                  "within [1, 6] dimensions (Eigen limit)."));
          }
        });
    AddComment(R"DOC(
    Unsqueeze Operator.

    Insert single-dimensional entries to the shape of a tensor.
    Takes one required argument axes, a list of dimensions that will be inserted.
    Dimension indices in axes are as seen in the output tensor.

    For example:
      Given a tensor such that tensor with shape [3, 4, 5],
      then Unsqueeze(tensor, axes=[0, 4]) has shape [1, 3, 4, 5, 1]
    )DOC");
  }
};

class UnsqueezeGradOp : public framework::OperatorWithKernel {
 public:
  using framework::OperatorWithKernel::OperatorWithKernel;

  void InferShape(framework::InferShapeContext *ctx) const override {
    ctx->SetOutputDim(framework::GradVarName("X"), ctx->GetInputDim("X"));
    ctx->ShareLoD("X", framework::GradVarName("X"));
  }

  framework::OpKernelType GetExpectedKernelType(
      const framework::ExecutionContext &ctx) const override {
    return framework::OpKernelType(OperatorWithKernel::IndicateVarDataType(
                                       ctx, framework::GradVarName("Out")),
                                   ctx.device_context());
  }
};

template <typename T>
class UnsqueezeGradOpMaker : public framework::SingleGradOpMaker<T> {
 public:
  using framework::SingleGradOpMaker<T>::SingleGradOpMaker;

  void Apply(GradOpPtr<T> grad_op) const override {
    grad_op->SetType("unsqueeze_grad");
    grad_op->SetInput("X", this->Input("X"));
    grad_op->SetInput(framework::GradVarName("Out"), this->OutputGrad("Out"));
    grad_op->SetOutput(framework::GradVarName("X"), this->InputGrad("X"));
    grad_op->SetAttrMap(this->Attrs());
  }
};

template <typename T>
class UnsqueezeDoubleGradOpMaker : public framework::SingleGradOpMaker<T> {
 public:
  using framework::SingleGradOpMaker<T>::SingleGradOpMaker;

  void Apply(GradOpPtr<T> grad_op) const override {
    grad_op->SetType("unsqueeze");
    grad_op->SetInput("X", this->OutputGrad(framework::GradVarName("X")));
    grad_op->SetOutput("Out", this->InputGrad(framework::GradVarName("Out")));
    grad_op->SetAttrMap(this->Attrs());
  }
};

DECLARE_INPLACE_OP_INFERER(UnsqueezeInplaceInferer, {"X", "Out"});
DECLARE_INPLACE_OP_INFERER(UnsqueezeGradInplaceInferer,
                           {framework::GradVarName("Out"),
                            framework::GradVarName("X")});
DECLARE_NO_NEED_BUFFER_VARS_INFERER(UnsqueezeGradOpNoNeedBufferVarInferer, "X");
}  // namespace operators
}  // namespace paddle

namespace ops = paddle::operators;
REGISTER_OPERATOR(unsqueeze,
                  ops::UnsqueezeOp,
                  ops::UnsqueezeOpMaker,
                  ops::UnsqueezeGradOpMaker<paddle::framework::OpDesc>,
                  ops::UnsqueezeGradOpMaker<paddle::imperative::OpBase>);

REGISTER_OPERATOR(unsqueeze_grad,
                  ops::UnsqueezeGradOp,
                  ops::UnsqueezeDoubleGradOpMaker<paddle::framework::OpDesc>,
                  ops::UnsqueezeDoubleGradOpMaker<paddle::imperative::OpBase>,
                  ops::UnsqueezeGradOpNoNeedBufferVarInferer);

REGISTER_OP_CPU_KERNEL(
    unsqueeze,
    ops::UnsqueezeKernel<phi::CPUContext, float>,
    ops::UnsqueezeKernel<phi::CPUContext, double>,
    ops::UnsqueezeKernel<phi::CPUContext, bool>,
    ops::UnsqueezeKernel<phi::CPUContext, int>,
    ops::UnsqueezeKernel<phi::CPUContext, int16_t>,
    ops::UnsqueezeKernel<phi::CPUContext, uint8_t>,
    ops::UnsqueezeKernel<phi::CPUContext, int8_t>,
    ops::UnsqueezeKernel<phi::CPUContext, int64_t>,
    ops::UnsqueezeKernel<phi::CPUContext, paddle::platform::complex<float>>,
    ops::UnsqueezeKernel<phi::CPUContext, paddle::platform::complex<double>>,
    ops::UnsqueezeKernel<phi::CPUContext, paddle::platform::bfloat16>);
REGISTER_OP_CPU_KERNEL(
    unsqueeze_grad,
    ops::UnsqueezeGradKernel<phi::CPUContext, float>,
    ops::UnsqueezeGradKernel<phi::CPUContext, double>,
    ops::UnsqueezeGradKernel<phi::CPUContext, bool>,
    ops::UnsqueezeGradKernel<phi::CPUContext, int>,
    ops::UnsqueezeGradKernel<phi::CPUContext, int16_t>,
    ops::UnsqueezeGradKernel<phi::CPUContext, uint8_t>,
    ops::UnsqueezeGradKernel<phi::CPUContext, int8_t>,
    ops::UnsqueezeGradKernel<phi::CPUContext, int64_t>,
    ops::UnsqueezeGradKernel<phi::CPUContext, paddle::platform::complex<float>>,
    ops::UnsqueezeGradKernel<phi::CPUContext,
                             paddle::platform::complex<double>>,
    ops::UnsqueezeGradKernel<phi::CPUContext, paddle::platform::bfloat16>);
