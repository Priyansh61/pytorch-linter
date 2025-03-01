#include <torch/csrc/jit/codegen/cuda/interface.h>

#include <ATen/core/dispatch/OperatorOptions.h>
#include <c10/util/irange.h>
#include <torch/csrc/jit/runtime/custom_operator.h>
#include <torch/csrc/jit/runtime/register_ops_utils.h>

// NOLINTNEXTLINE
C10_DEFINE_bool(
    torch_jit_nvfuser_singleton_fusion,
    false,
    "enable single node fusion for nvfuser");

// NOLINTNEXTLINE
C10_DEFINE_bool(
    torch_jit_nvfuser_horizontal_fusion,
    true,
    "enable horizontal fusion for nvfuser");

namespace torch {
namespace jit {
namespace fuser {
namespace cuda {

static std::atomic<bool> cuda_fusion_guard_mode{true};

// There are 3 sources of information on whether to enable nvfuser:
// 1. assigned value from setEnabled() - takes precendence if it has been set
// 2. value from environment variable - only used if setEnabled() is unset
// 3. default value - used if both 1 and 2 are unset.
//
// If 1 or 2 tries to enable nvfuser when it cannot be enabled (e.g. cuda not
// available), then an error will be thrown. The default will not error.
class NVFuserEnabler {
 private:
  c10::optional<bool> runtime_assigned_fuser_enabled_ = c10::nullopt;
  std::once_flag enabled_check_flag_;
  std::mutex mutex_;

  static bool nvfuserCanBeEnabled() {
#ifdef USE_ROCM
    return false;
#else
    return at::globalContext().hasCUDA() &&
        NVFuserPassManager::isRegistered() && getExecutorMode();
#endif
  }

  static void assertFuserCanBeEnabled(bool is_enabled) {
    if (!is_enabled) {
      return;
    }
    TORCH_CHECK(
        nvfuserCanBeEnabled(),
        "Running CUDA fuser is only supported on CUDA builds.");
  }

  static c10::optional<bool> getFuserEnabledEnvVar() {
    static const char* enable_c_str = std::getenv("PYTORCH_JIT_ENABLE_NVFUSER");
    if (!enable_c_str) {
      return c10::nullopt;
    }
    std::string enable(enable_c_str);
    if (enable == "0" || enable == "OFF") {
      return false;
    }
    return true;
  }

  static c10::optional<bool> getCachedFuserEnabledEnvVar() {
    static c10::optional<bool> default_enabled = getFuserEnabledEnvVar();
    return default_enabled;
  }

  static bool getNNCNotNVFuser() {
    static const char* env_c_str =
        std::getenv("PYTORCH_JIT_USE_NNC_NOT_NVFUSER");
    if (!env_c_str) {
      return false;
    }
    std::string env(env_c_str);
    if (env == "1" || env == "ON") {
      return true;
    }
    return false;
  }

  static bool getCachedNNCNotNVFuser() {
    static bool force_disable = getNNCNotNVFuser();
    return force_disable;
  }

  bool isEnabledImpl() {
    std::call_once(enabled_check_flag_, [&]() {
      // if environment variable is setting the value, we must
      if (!runtime_assigned_fuser_enabled_.has_value() &&
          getCachedFuserEnabledEnvVar().has_value()) {
        assertFuserCanBeEnabled(*getCachedFuserEnabledEnvVar());
      }
    });
    // 0. opportunity to force disable NVFuser
    if (getCachedNNCNotNVFuser()) {
      return false;
    }
    // 1. if user has explicitly assigned fuser value, that value takes
    // precedence.
    if (runtime_assigned_fuser_enabled_.has_value()) {
      return *runtime_assigned_fuser_enabled_;
    }
    // 2. next precedence is any value assigned by
    if (getCachedFuserEnabledEnvVar().has_value()) {
      return *getCachedFuserEnabledEnvVar();
    }
    // 3. default value
#ifdef FBCODE_CAFFE2
    return false;
#else
    return nvfuserCanBeEnabled();
#endif
  }

 public:
  bool setEnabled(bool is_enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    assertFuserCanBeEnabled(is_enabled);
    bool old_value = isEnabledImpl();
    runtime_assigned_fuser_enabled_ = is_enabled;
    return old_value;
  }

  bool isEnabled() {
    std::lock_guard<std::mutex> lock(mutex_);
    return isEnabledImpl();
  }
};

static NVFuserEnabler nvfuser_enabler;

bool isEnabled() {
  return nvfuser_enabler.isEnabled();
}

bool setEnabled(bool is_enabled) {
  return nvfuser_enabler.setEnabled(is_enabled);
}

bool getSingletonFusion() {
  return FLAGS_torch_jit_nvfuser_singleton_fusion;
}

bool setSingletonFusion(bool value) {
  bool old_value = FLAGS_torch_jit_nvfuser_singleton_fusion;
  FLAGS_torch_jit_nvfuser_singleton_fusion = value;
  return old_value;
}

bool getHorizontalFusion() {
  return FLAGS_torch_jit_nvfuser_horizontal_fusion;
}

bool setHorizontalFusion(bool value) {
  bool old_value = FLAGS_torch_jit_nvfuser_horizontal_fusion;
  FLAGS_torch_jit_nvfuser_horizontal_fusion = value;
  return old_value;
}

std::atomic<bool>& getCudaFusionGuardMode() {
  return cuda_fusion_guard_mode;
}

CudaFuserInterface* getFuserInterface() {
  static CudaFuserInterface fuser_interface_;
  return &fuser_interface_;
}

void compileFusionGroup(Node* fusion_node) {
  TORCH_CHECK(
      getFuserInterface()->fn_compile_n != nullptr,
      "Running the CUDA fuser requires a CUDA build.");
  getFuserInterface()->fn_compile_n(fusion_node);
}

void runFusionGroup(const Node* fusion_node, Stack& stack) {
  TORCH_CHECK(
      getFuserInterface()->fn_run_n_s != nullptr,
      "Running the CUDA fuser requires a CUDA build.");
  getFuserInterface()->fn_run_n_s(fusion_node, stack);
}

void fuseGraph(std::shared_ptr<Graph>& graph) {
  if (!isEnabled()) {
    return;
  }

  TORCH_CHECK(
      getFuserInterface()->fn_fuse_graph != nullptr,
      "Running the CUDA fuser requires a CUDA build.");
  getFuserInterface()->fn_fuse_graph(graph);
}

bool canFuseNode(const Node* node) {
  return getFuserInterface()->fn_can_fuse_n != nullptr &&
      getFuserInterface()->fn_can_fuse_n(node);
}

void InsertProfileNodesForCUDAFuser(ProfilingRecord* pr) {
  if (getFuserInterface()->fn_insert_profile_inodes) {
    getFuserInterface()->fn_insert_profile_inodes(pr);
  }
}

bool profileNode(const Node* node) {
  return getFuserInterface()->fn_profile_n != nullptr &&
      getFuserInterface()->fn_profile_n(node);
}

bool skipNode(const std::string& symbol_str, bool flip) {
  return getFuserInterface()->fn_skip_n != nullptr &&
      getFuserInterface()->fn_skip_n(symbol_str, flip);
}

//! [ Note -- type guard logic in CudaFusionGuard ]
//!
//! CudaFusionGuard is used to Guard input tensor to `CudaFusionGroup` so that
//! we would not feed inputs that violates the graph defined in `GraphCache`.
//!
//! see [ Note -- 2 level cache implementation ] for definition of unique
//! computational graph.
//! see [ Note -- CudaFusionGuard implementation] for details on how guard works
//! in profiling executor
//!
//! Type guard logic is used to query whether a runtime input `tensor` compiles
//! with profiled `guard_tensor_type`. `guard_tensor_type` is the observed
//! tensor type during profiling runs.
//!
//! At this moment, we only do single profiling run, so `guard_tensor_type` has
//! static shape / stride / scalarType. *This might be a little confusing as our
//! implementation is actually more relaxed.
//!
//! Things that we check:
//!   a. identical rank & scalar type
//!   b. stride check:
//!        b.1. identical stride order
//!        b.2. identical contiguity
//!             note that contiguity here is used for tensor collapsing. So
//!             extra attention should be paid to contiguity across size-1
//!             dimensions.
//!   c. size check:
//!        c.1 broadcast check:
//!        making sure that broadcast semantics are identical. So we want to
//!        make sure a given dimension either are both size-1 for `tensor` &
//!        `guard_tensor_type`, or are both non-size-1.
//!        This is due to the fact that we specialize size-1 dimension as
//!        broadcasted dimension while translating PyTorch tensor to Fusion IR.
//!        c.1 size-0 check:
//!        we don't specialize this on codegen, but we do specialize fusion
//!        logic for size-0 on reductoins, hence the check
//!
bool complyWith(
    const at::Tensor& tensor,
    const c10::TensorTypePtr& guard_tensor_type) {
  // guard broadcast semantics, contiguity & stride order;
  TORCH_INTERNAL_ASSERT(
      guard_tensor_type && guard_tensor_type->dim().has_value());

  // check a. if num_dimension check fails or scalar type check fails
  if (*guard_tensor_type->dim() != static_cast<size_t>(tensor.ndimension()) ||
      (guard_tensor_type->scalarType().has_value() &&
       (guard_tensor_type->scalarType().value() != tensor.scalar_type())) ||
      (guard_tensor_type->device().has_value() &&
       (guard_tensor_type->device().value() != tensor.device())) ||
      (guard_tensor_type->requiresGrad().has_value() &&
       guard_tensor_type->requiresGrad().value() !=
           (tensor.requires_grad() && at::GradMode::is_enabled()))) {
    return false;
  }

  // TODO: should we get symbolic_size instead and check for size
  // consistency across tensors as well?
  const auto& sizes = guard_tensor_type->sizes();
  // see [ Note -- stirde_properties in tensor type ]
  const auto& stride_properties = guard_tensor_type->stride_properties();

  const auto& t_sizes = tensor.sizes();
  const auto& t_strides = tensor.strides();
  int inner_dim = -1;
  for (const auto j : c10::irange(*guard_tensor_type->dim())) {
    // check b. for stride check, we go along dimensions from fastest stride to
    // slowest stride
    int sorted_index = stride_properties[j]->stride_index_
        ? static_cast<int>(*stride_properties[j]->stride_index_)
        : -1;

    // only apply stride check when we have stride_properties
    if (sorted_index != -1) {
      // check b.1. stride order [current dimension has stride larger
      // than its inner dimension(s)], check only applies when both:
      //     i. already encountered an inner dimension
      //    ii. not at the fastest dimension
      if (j != 0 && inner_dim != -1) {
        // we are not looking at dim-j, but dim-sorted_index, which
        // is the j-th fastest dim;
        // Note: we ignore 0-stride dimension, since eager logic on stride
        // indices is ambiguous
        if (t_strides[sorted_index] != 0 && t_strides[inner_dim] != 0 &&
            t_strides[sorted_index] < t_strides[inner_dim]) {
          return false;
        }
      }

      // check b.2. contiguity, we only check when it's marked as
      // contiguous.
      if (stride_properties[j]->contiguous_ &&
          *stride_properties[j]->contiguous_) {
        if (j != 0) {
          // we use contiguity to collapse dimension, if size == 1, it is
          // always collapsible
          // computeStrideProps also default to contiguous when stride == 1
          if (t_sizes[sorted_index] != 1 && t_strides[sorted_index] != 1) {
            TORCH_INTERNAL_ASSERT(
                stride_properties[j - 1]->stride_index_.has_value(),
                "Counknown index is meaningless");
            // TODO: merge this check up
            if (t_strides[sorted_index] !=
                t_strides[inner_dim] * t_sizes[inner_dim]) {
              return false;
            }
          }
        } else {
          // TODO: merge this check up
          if (t_strides[sorted_index] != 1) {
            return false;
          }
        }
      }

      // update inner_dim to be current dim. Note that we try to skip update
      // when current `t_size[sorted_index] == 1`, because:
      //   1. stride comparison on a size-1 dimension is meaningless
      //      [check b.1]
      //   2. contiguity on a size-1 dimension is misleading. For collapsing,
      //      we should actually look at the next non-size-1 dimension
      //      [check b.2]
      if (inner_dim == -1 || t_sizes[sorted_index] != 1) {
        inner_dim = sorted_index;
      }
    }

    // check c.1, we go along semantic ordered dimensions
    // check broadcast / size-1:
    bool guard_bcast = sizes[j].has_value() && sizes[j].value() == 1;
    if (guard_bcast != (t_sizes[j] == 1)) {
      return false;
    }

    // check c.2, check for size-0
    bool guard_size_0 = sizes[j].has_value() && sizes[j].value() == 0;
    if (guard_size_0 != (t_sizes[j] == 0)) {
      return false;
    }
  }

  return true;
}

} // namespace cuda
} // namespace fuser

namespace {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
RegisterOperators size_eq_guard({
    Operator(
        //"prim::CudaFusionSizeEq(int[] size, int[] ref) -> bool",
        "prim::CudaFusionSizeEq(...) -> bool",
        // prim::CudaFusionGuard returns a fresh Boolean type without aliasing.
        // if we would ever return refined tensor, which would change aliasing
        // analysis, we should update aliasdb pass.
        [](const Node* node) -> Operation {
          return [](Stack& stack) {
            at::ArrayRef<IValue> inputs = last(stack, 2);
            drop(stack, 2);

            if (!fuser::cuda::getCudaFusionGuardMode()) {
              push(stack, IValue(true));
              return;
            }

            // auto inp = inputs[0].toIntList();
            TORCH_INTERNAL_ASSERT(
                inputs[1].isIntList(), "reference needs to be of int list");
            auto ref = inputs[1].toIntList();

            auto ret = true;
            if (ref.empty()) {
              ret = inputs[0].isNone();
            } else {
              if (inputs[0].isIntList()) {
                auto inp = inputs[0].toIntList();
                if (inp.size() != ref.size()) {
                  push(stack, IValue(false));
                  return;
                }

                for (const auto i : c10::irange(inp.size())) {
                  if (((inp[i] == 1) != (ref[i] == 1))) {
                    ret = false;
                    break;
                  }
                }
              } else {
                ret = false;
              }
            }

            push(stack, IValue(ret));
            return;
          };
        },
        aliasAnalysisFromSchema()),
});

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
RegisterOperators reg_fusion({
    Operator(
        prim::CudaFusionGroup,
        [](const Node* node) -> Operation {
          return [node](Stack& stack) {
            fuser::cuda::runFusionGroup(node, stack);
          };
        },
        aliasAnalysisSpecialCase()),
});

RegisterOperators reg_guard({
    Operator(
        "prim::CudaFusionGuard(...) -> bool",
        // prim::CudaFusionGuard returns a fresh Boolean type without aliasing.
        // if we would ever return refined tensor, which would change aliasing
        // analysis, we should update aliasdb pass.
        [](const Node* node) -> Operation {
          return [node](Stack& stack) {
            // TODO: check latency here!!!!
            std::vector<TypePtr> types = node->tys(attr::types);
            const auto num_inputs = types.size();
            at::ArrayRef<IValue> inputs = last(stack, num_inputs);
            drop(stack, num_inputs);

            if (!fuser::cuda::getCudaFusionGuardMode()) {
              push(stack, IValue(true));
              return;
            }

            for (const auto i : c10::irange(num_inputs)) {
              const c10::TensorTypePtr& guard_tensor_type =
                  types[i]->cast<TensorType>();

              // TODO: maybe we should just push false and fallback
              TORCH_INTERNAL_ASSERT(inputs[i].isTensor());
              const at::Tensor& tensor = inputs[i].toTensor();

              if (!fuser::cuda::complyWith(tensor, guard_tensor_type)) {
                push(stack, IValue(false));
                return;
              }
            }

            // TODO: check type and return the right flag
            // naively return true;
            push(stack, IValue(true));
            return;
          };
        },
        aliasAnalysisFromSchema()),
});

// Infer dynamic axis (-1) in view_sizes given tensor_sizes
bool inferViewShape(
    c10::List<int64_t> tensor_sizes,
    c10::List<int64_t> view_sizes) {
  int64_t dynamic_index = -1;
  size_t view_size_num_elements = 1;
  for (size_t idx = 0; idx < view_sizes.size(); ++idx) {
    if (view_sizes[idx] == -1) {
      TORCH_INTERNAL_ASSERT(
          dynamic_index == -1, "Only one dimension can by inferred.")
      dynamic_index = idx;
    } else {
      TORCH_INTERNAL_ASSERT(view_sizes[idx] > 0);
      view_size_num_elements *= view_sizes[idx];
    }
  }
  const size_t kNumElements = std::accumulate(
      tensor_sizes.begin(), tensor_sizes.end(), 1, std::multiplies<>());

  if (kNumElements % view_size_num_elements != 0) {
    return false;
  }

  if (dynamic_index != -1) {
    view_sizes[dynamic_index] = kNumElements / view_size_num_elements;
  }

  return true;
}

//! [ Note -- type guard logic in CudaFusionViewGuard ]
//!
//! CudaFusionViewGuard is used to guard input tensors to a `CudaFusionGroup`
//! that contains view operations, so that we would not feed inputs that
//! violate the graph defined in `GraphCache`.
//!
//! output = view(self, view-sizes)
//!
//! View Guard Inputs:
//!   1. self tensor_sizes - dynamic size List[Int]
//!   2. view_sizes - profile_ivalue List[Int]
//!   3. tensor_constraint - Constant List[Int]
//!   4. view_sizes_constraint - Constant List[Int]
//!
//! Things that we check:
//!   1. The #dimensions are the same for self tensor and its constraint
//!   2. The #dimensions are the same for view-sizes and its constraint
//!   3. Self tensor does not violate its constraint
//!     a. Queue unrestricted sizes
//!     b. Calculate #elements in self tensor
//!   4. view-sizes does not violate its constraint
//!     a. Pop unrestricted sizes from queue
//!     b. Calculate #elements in view-sizes
//!   5. The #elements is the same for self tensor and view-sizes
//!
//! Constraints:
//! A restricted axis creates a graph constraint, so its sizes is static.
//! An unrestricted axis is allowed to have a dynamic size, if it is consistent
//! between self tensor and view-sizes. It is marked with -1 in the constraint.
//! Only iterDomains with the Keep transform are dynamic. All other transforms
//! create a static constraint.
//!
bool checkViewGuard(
    c10::List<int64_t> tensor_sizes,
    c10::List<int64_t> view_sizes,
    c10::List<int64_t> tensor_constraint,
    c10::List<int64_t> view_sizes_constraint) {
  // 1: Num Dimensions Check
  if (tensor_constraint.size() != tensor_sizes.size() ||
      view_sizes_constraint.size() != view_sizes.size()) {
    return false;
  }

  // If axis allows dynamic sizes, then add tensor size to this queue.
  // For dynamic axes in view_sizes, check that it is consistent with
  // the corresponding tensor size.
  std::queue<int64_t> dynamic_axis_queue;

  // 2. Tensor Static Check
  int64_t tensor_size_product = 1;
  for (const auto idx : c10::irange(tensor_sizes.size())) {
    if (tensor_constraint[idx] == -1) {
      dynamic_axis_queue.push(tensor_sizes[idx]);
    } else if (tensor_constraint[idx] != tensor_sizes[idx]) {
      return false;
    }
    tensor_size_product *= tensor_sizes[idx];
  }

  // 3. View-Sizes Static Check
  int64_t view_size_product = 1;
  for (const auto idx : c10::irange(view_sizes.size())) {
    auto dynamic_size = (view_sizes_constraint[idx] == -1)
        ? dynamic_axis_queue.front()
        : view_sizes_constraint[idx];
    if (dynamic_size != view_sizes[idx]) {
      return false;
    }
    view_size_product *= dynamic_size;
    if (view_sizes_constraint[idx] == -1) {
      dynamic_axis_queue.pop();
    }
  }

  // 4. Check view invariant
  // The number of elements in the input and output tensors are the same.
  return tensor_size_product == view_size_product;
}

//!
//! CudaFusionViewGuard Example Graph:
//!
//! graph(%self : __torch__.BiasViewRelu,
//!       %inputs.1 : Tensor):
//!   %2 : int = prim::Constant[value=-1]() # dynamic_bvg.py:50:40
//!   %3 : int = prim::Constant[value=1]() # dynamic_bvg.py:50:25
//!   %4 : NoneType = prim::Constant()
//!   %5 : int[] = prim::Constant[value=[2, 3]]()
//!   %6 : int[] = aten::size(%inputs.1) # dynamic_bvg.py:50:25
//!   %7 : int[] = aten::slice(%6, %4, %2, %3) # dynamic_bvg.py:50:25
//!   %view_shape.1 : int[] = aten::add(%7, %5) # dynamic_bvg.py:50:25
//!   %bias : Tensor = prim::GetAttr[name="bias"](%self)
//!   %10 : int[] = aten::size(%bias)
//!   %11 : int[] = prim::BroadcastSizes(%6, %10)
//!   %12 : bool = prim::CudaFusionGuard[types=[...]](%inputs.1, %bias)
//!   %13 : int[] = prim::Constant[value=[-1, -1, -1, 6]]()
//!   %14 : int[] = prim::Constant[value=[-1, -1, -1, 2, 3]]()
//!   %15 : bool = prim::CudaFusionViewGuard(%11, %view_shape.1, %13, %14)
//!   %16 : bool[] = prim::ListConstruct(%15, %12)
//!   %17 : bool = aten::all(%16)
//!   %18 : Tensor = prim::If(%17)
//!     block0():
//!       %19 : Tensor = prim::CudaFusionGroup_0[cache_id=0](%inputs.1, %bias)
//!       -> (%19)
//!     block1():
//!       %20 : Function = prim::Constant[name="fallback_fn", fallback=1]()
//!       %21 : (...) = prim::CallFunction(%20, %inputs.1, %bias, %view_shape.1)
//!       %22 : Float(...) = prim::TupleUnpack(%21)
//!       -> (%22)
//!   return (%18)
//! with prim::CudaFusionGroup_0 = graph(%0 : Float(...),
//!       %1 : Float(...)):
//!   %2 : int[] = prim::Constant[value=[2, 3, 4, 2, 3]]()
//!   %3 : int = prim::Constant[value=1]() # dynamic_bvg.py:50:25
//!   %o.1 : Float(...) = aten::add(%0, %1, %3) # dynamic_bvg.py:51:16
//!   %5 : Float(...) = prim::view_copy(%o.1, %2)
//!   %6 : Float(...) = aten::relu(%5) # dynamic_bvg.py:53:19
//!   return (%6)
//!
RegisterOperators view_guard({
    Operator(
        "prim::CudaFusionViewGuard(...) -> bool",
        // prim::CudaFusionViewGuard returns a fresh Boolean type without
        // aliasing. if we would ever return refined tensor, which would change
        // aliasing analysis, we should update aliasdb pass.
        [](const Node* node) -> Operation {
          return [](Stack& stack) {
            // view_sizes_constraint - Constant List[Int]
            at::ArrayRef<IValue> inputs = last(stack, 4);

            // tensor_sizes is the runtime size for the self tensor
            // tensor_sizes - dynamic size List[Int]
            TORCH_INTERNAL_ASSERT(
                inputs[0].isIntList(), "tensor_sizes needs to be Int List");
            auto tensor_sizes = inputs[0].toIntList();

            // profiled_view_sizes is the runtime view size
            // profiled_view_sizes - profile_ivalue List[Int]
            TORCH_INTERNAL_ASSERT(
                inputs[1].isIntList(),
                "profiled_view_sizes needs to be Int list");
            auto profiled_view_sizes = inputs[1].toIntList();

            // tensor_constraint is a constant List[Int]
            // used to guard tensor_sizes
            TORCH_INTERNAL_ASSERT(
                inputs[2].isIntList(),
                "tensor constraint needs to be Int List");
            auto tensor_constraint = inputs[2].toIntList();

            // view_sizes_constraint is a constant List[Int]
            // used to guard profiled_view_sizes
            TORCH_INTERNAL_ASSERT(
                inputs[3].isIntList(),
                "view_sizes constraint needs to be Int List");
            auto view_sizes_constraint = inputs[3].toIntList();

            // Drop after gather all input arguments
            // If an argument is moved, it is destroyed when dropped from stack
            drop(stack, 4);

            auto status = inferViewShape(tensor_sizes, profiled_view_sizes);
            if (!status) {
              push(stack, IValue(false));
              return;
            }

            if (!fuser::cuda::getCudaFusionGuardMode()) {
              push(stack, IValue(true));
              return;
            }

            auto guard_status = checkViewGuard(
                tensor_sizes,
                profiled_view_sizes,
                tensor_constraint,
                view_sizes_constraint);
            push(stack, IValue(guard_status));
            return;
          };
        },
        aliasAnalysisFromSchema()),
});

RegisterOperators ivalue_guard({
    Operator(
        "prim::CudaFusionIvalGuard(...) -> bool",
        [](const Node* node) -> Operation {
          return [](Stack& stack) {
            at::ArrayRef<IValue> inputs = last(stack, 2);
            drop(stack, 2);
            if (!fuser::cuda::getCudaFusionGuardMode()) {
              push(stack, IValue(true));
              return;
            }
            push(stack, inputs[0].equals(inputs[1]));
            return;
          };
        },
        aliasAnalysisFromSchema()),
});

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
RegisterOperators reg_add_optional({
    Operator(
        "prim::add_optional(Tensor(a) input, Tensor? bias) -> Tensor(a)",
        [](const Node* node) -> Operation {
          return [](Stack& stack) {
            IValue input, bias;
            pop(stack, input, bias);
            if (bias.isNone()) {
              push(stack, std::move(input));
            } else {
              push(stack, at::add(input.toTensor(), bias.toTensor(), 1.0));
            }
          };
        },
        aliasAnalysisFromSchema()),
});

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
RegisterOperators reg_view_copy({
    Operator(
        "prim::view_copy(Tensor self, int[] size) -> Tensor",
        [](const Node* node) -> Operation {
          return [node](Stack& stack) {
            TORCH_CHECK(
                node->s(attr::name) == "CudaFusionGroup",
                "view_copy is only used by nvfuser to identify non-mutating ",
                "alias ops, should be restored after fusion pass!");
            IValue self, size;
            pop(stack, self, size);
            push(stack, at::native::view(self.toTensor(), size.toIntVector()));
          };
        },
        aliasAnalysisFromSchema()),
});

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
RegisterOperators reg_reshape_copy({
    Operator(
        "prim::reshape_copy(Tensor self, int[] shape) -> Tensor",
        [](const Node* node) -> Operation {
          return [node](Stack& stack) {
            TORCH_CHECK(
                node->s(attr::name) == "CudaFusionGroup",
                "reshape_copy is only used by nvfuser to identify non-mutating ",
                "alias ops, should be restored after fusion pass!");
            IValue self, shape;
            pop(stack, self, shape);
            push(
                stack,
                at::native::reshape(self.toTensor(), shape.toIntVector()));
          };
        },
        aliasAnalysisFromSchema()),
});

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
RegisterOperators reg_squeeze_copy({
    Operator(
        "prim::squeeze_copy(Tensor self) -> Tensor",
        [](const Node* node) -> Operation {
          return [node](Stack& stack) {
            TORCH_CHECK(
                node->s(attr::name) == "CudaFusionGroup",
                "squeeze_copy is only used by nvfuser to identify non-mutating ",
                "alias ops, should be restored after fusion pass!");
            IValue self;
            pop(stack, self);
            push(stack, at::squeeze(self.toTensor()));
          };
        },
        aliasAnalysisFromSchema()),
});

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
RegisterOperators reg_squeeze_dim_copy({
    Operator(
        "prim::squeeze_copy.dim(Tensor self, int dim) -> Tensor",
        [](const Node* node) -> Operation {
          return [node](Stack& stack) {
            TORCH_CHECK(
                node->s(attr::name) == "CudaFusionGroup",
                "squeeze_dim_copy is only used by nvfuser to identify non-mutating ",
                "alias ops, should be restored after fusion pass!");
            IValue self, dim;
            pop(stack, self, dim);
            push(stack, at::squeeze(self.toTensor(), dim.toInt()));
          };
        },
        aliasAnalysisFromSchema()),
});

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
RegisterOperators reg_unsqueeze_copy({
    Operator(
        "prim::unsqueeze_copy(Tensor self, int dim) -> Tensor",
        [](const Node* node) -> Operation {
          return [node](Stack& stack) {
            TORCH_CHECK(
                node->s(attr::name) == "CudaFusionGroup",
                "unsqueeze_copy is only used by nvfuser to identify non-mutating ",
                "alias ops, should be restored after fusion pass!");
            IValue self, dim;
            pop(stack, self, dim);
            push(stack, at::unsqueeze(self.toTensor(), dim.toInt()));
          };
        },
        aliasAnalysisFromSchema()),
});

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
RegisterOperators reg_infer_unsqueeze_size({
    Operator(
        "prim::infer_unsqueeze_size(int[] a, int dim) -> int[]",
        [](const Node* node) -> Operation {
          return [](Stack& stack) {
            auto dim = pop(stack).toInt();
            auto size = pop(stack).toIntVector();
            if (dim < 0) {
              dim = dim + 1 + size.size();
            }
            auto it = size.begin() + dim;
            size.insert(it, 1);
            push(stack, IValue(size));
          };
        },
        aliasAnalysisFromSchema()),
});

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
RegisterOperators reg_infer_squeeze_dim_size({
    Operator(
        "prim::infer_squeeze_size.dim(int[] a, int dim) -> int[]",
        [](const Node* node) -> Operation {
          return [](Stack& stack) {
            auto dim = pop(stack).toInt();
            auto size = pop(stack).toIntVector();
            if (dim < 0) {
              dim = dim + size.size();
            }
            auto it = size.begin() + dim;
            if (*it == 1) {
              size.erase(it);
            }
            push(stack, IValue(size));
          };
        },
        aliasAnalysisFromSchema()),
});

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
RegisterOperators reg_infer_squeeze_size({
    Operator(
        "prim::infer_squeeze_size(int[] a) -> int[]",
        [](const Node* node) -> Operation {
          return [](Stack& stack) {
            auto size = pop(stack).toIntVector();

            for (auto it = size.begin(); it != size.end(); it++) {
              if (*it == 1) {
                auto pre = it - 1;
                size.erase(it);
                it = pre;
              }
            }
            push(stack, IValue(size));
          };
        },
        aliasAnalysisFromSchema()),
});

} // namespace

} // namespace jit
} // namespace torch
