#include <torch/csrc/utils/python_dispatch.h>
#include <torch/csrc/jit/frontend/function_schema_parser.h>

#include <torch/library.h>
#include <ATen/ATen.h>
#include <ATen/core/dispatch/Dispatcher.h>

#include <c10/core/SafePyObject.h>
#include <torch/csrc/jit/python/pybind_utils.h>
#include <torch/csrc/autograd/python_variable.h>

#include <pybind11/operators.h>
#include <pybind11/stl.h>

#include <iostream>

namespace py = pybind11;

namespace torch {
namespace impl {
namespace dispatch {

torch::Library::Kind parseKind(const std::string& k) {
  static std::unordered_map<std::string, torch::Library::Kind> kind_map = {
    {"DEF", torch::Library::DEF},
    {"IMPL", torch::Library::IMPL},
    {"FRAGMENT", torch::Library::FRAGMENT},
  };
  auto it = kind_map.find(k);
  TORCH_CHECK(it != kind_map.end(), "could not parse ", k);
  return it->second;
}
c10::AliasAnalysisKind parseAliasAnalysisKind(const std::string& k) {
  static std::unordered_map<std::string, c10::AliasAnalysisKind> key_map = {
    {"CONSERVATIVE", c10::AliasAnalysisKind::CONSERVATIVE},
    {"FROM_SCHEMA", c10::AliasAnalysisKind::FROM_SCHEMA},
    {"PURE_FUNCTION", c10::AliasAnalysisKind::PURE_FUNCTION},
    {"", c10::AliasAnalysisKind::FROM_SCHEMA},  // default
  };
  auto it = key_map.find(k);
  TORCH_CHECK(it != key_map.end(), "could not parse ", k);
  return it->second;
}


template <typename Func>
inline torch::CppFunction dispatch_str(const char* key, Func&& raw_f) {
  auto mb_key = std::string(key) == "" ? c10::nullopt : c10::make_optional(c10::parseDispatchKey(key));
  if (mb_key) {
    return torch::dispatch(*mb_key, std::forward<Func>(raw_f));
  } else {
    torch::CppFunction f(std::forward<Func>(raw_f));
    return f;
  }
}

class PythonKernelHolder : public c10::OperatorKernel {
  c10::SafePyObject func_;
public:
  PythonKernelHolder(py::object func) : func_(func.release().ptr(), getPyInterpreter()) {}

  void operator()(const c10::OperatorHandle& op, c10::DispatchKeySet keyset, torch::jit::Stack* stack) {
    auto arguments = torch::jit::pop(*stack, op.schema().arguments().size());
    py::gil_scoped_acquire g;
    auto args_kwargs = parseIValuesToPyArgsKwargs(op, arguments);
    auto obj = py::reinterpret_steal<py::object>(PyObject_Call(func_.ptr(getPyInterpreter()), args_kwargs.first.ptr(), args_kwargs.second.ptr()));
    if (obj == nullptr) { throw python_error(); }
    pushPyOutToStack(op, stack, obj, "PythonKernelHolder");
  }
};

void initDispatchBindings(PyObject* module) {
  auto m = py::handle(module).cast<py::module>();

  py::class_<c10::OperatorHandle>(m, "_DispatchOperatorHandle")
    .def("schema", &c10::OperatorHandle::schema);

  // TODO: figure out how to do chaining
  py::class_<torch::Library>(m, "_DispatchModule")
    .def("def_", [](py::object self, const char* schema, const char* alias) {
      self.cast<torch::Library&>().def(torch::schema(schema, parseAliasAnalysisKind(alias)));
      return self;
    }, "", py::arg("schema"), py::arg("alias") = "")
    // Simulated "legacy" def where alias analysis kind is not set.
    // Ordinarily this can only be exercised from RegisterOperators() API
    // but I am not going to bind that here
    .def("def_legacy", [](py::object self, const char* schema) {
      self.cast<torch::Library&>().def(torch::jit::parseSchema(schema));
      return self;
    }, "", py::arg("schema"))
    // We can't conveniently turn Python functions into valid functions
    // in the dispatcher.  So instead we provide a bunch of precanned
    // functions for testing purposes.  You're NOT intended to actually
    // call these functions; they're just here so we can actually register
    // something
    //
    // Mangling scheme: args_rets.  One character per.
    //  t = Tensor
    .def("def_name_t_t", [](py::object self, const char* name, const char* dispatch, const char* debug) {
      self.cast<torch::Library&>().def(
        name,
        dispatch_str(dispatch, [](const at::Tensor& a) {
          return a;
        }).debug(debug)
      );
      return self;
    }, "", py::arg("name"),
           py::arg("dispatch") = "",
           py::arg("debug") = "default_def_name_t_t")
    .def("def_schema_t_t", [](py::object self, const char* schema, const char* dispatch, const char* alias, const char* debug) {
      self.cast<torch::Library&>().def(
        torch::schema(schema, parseAliasAnalysisKind(alias)),
        dispatch_str(dispatch, [](const at::Tensor& a) {
          return a;
        }).debug(debug)
      );
      return self;
    }, "", py::arg("name"),
           py::arg("dispatch") = "",
           py::arg("alias") = "",
           py::arg("debug") = "default_def_schema_t_t")
    // TODO: maybe consider deduplicating the definitions here, it's getting
    // pretty long
    .def("impl_t_t", [](py::object self, const char* name, const char* dispatch, const char* debug) {
      self.cast<torch::Library&>().impl(
        name,
        dispatch_str(dispatch, [](const at::Tensor& a) {
          return a;
        }).debug(debug)
      );
      return self;
    }, "", py::arg("name"),
           py::arg("dispatch") = "",
           py::arg("debug") = "impl_t_t")
    .def("impl_tt_t", [](py::object self, const char* name, const char* dispatch, const char* debug) {
      self.cast<torch::Library&>().impl(
        name,
        dispatch_str(dispatch, [](const at::Tensor& a, const at::Tensor& b) {
          return a;
        }).debug(debug)
      );
      return self;
    }, "", py::arg("name"), py::arg("dispatch") = "", py::arg("debug") = "")
    .def("impl", [](py::object self, const char* name, const char* dispatch, py::object func) {
      HANDLE_TH_ERRORS
      self.cast<torch::Library&>().impl(
        name,
        dispatch_str(dispatch, CppFunction::makeFromBoxedFunctor(std::make_unique<PythonKernelHolder>(std::move(func))))
      );
      END_HANDLE_TH_ERRORS_PYBIND
    }, "", py::arg("name"), py::arg("dispatch"), py::arg("func"))
    .def("define", [](py::object self, const char* schema) {
      self.cast<torch::Library&>().def(torch::schema(schema, c10::AliasAnalysisKind::FROM_SCHEMA));
    }, "", py::arg("schema"))
    .def("fallback_fallthrough", [](py::object self, const char* dispatch) {
      self.cast<torch::Library&>().fallback(
        dispatch_str(dispatch, CppFunction::makeFallthrough())
      );
      return self;
    }, "", py::arg("dispatch") = "")
  ;

  m.def("_dispatch_library", [](const char* kind, std::string name, const char* dispatch, const char* file, uint32_t linenum) {
    HANDLE_TH_ERRORS
    return std::make_unique<torch::Library>(
      parseKind(kind),
      std::move(name),
      std::string(dispatch) == "" ? c10::nullopt : c10::make_optional(c10::parseDispatchKey(dispatch)),
      file,
      linenum);
    END_HANDLE_TH_ERRORS_PYBIND
  }, "", py::arg("kind"), py::arg("name"), py::arg("dispatch"), py::arg("file")="/dev/null", py::arg("linenum")=0)
  ;

  m.def("_dispatch_dump", [](const char* name) -> std::string {
    auto op = c10::Dispatcher::singleton().findOp(torch::jit::parseName(name));
    if (!op) {
      return "";
    } else {
      return op->dumpState();
    }
  });

  m.def("_dispatch_dump_table", [](const char* name) -> std::string {
    auto op = c10::Dispatcher::singleton().findOp(torch::jit::parseName(name));
    if (!op) {
      return "";
    } else {
      return op->dumpComputedTable();
    }
  });

  m.def("_dispatch_check_invariants", [](const char* name) {
    auto op = c10::Dispatcher::singleton().findOp(torch::jit::parseName(name));
    if (!op) {
    } else {
      return op->checkInvariants();
    }
  });

  m.def("_dispatch_check_all_invariants", []() {
    c10::Dispatcher::singleton().checkInvariants();
  });

  m.def("_dispatch_find_dangling_impls", []() -> std::vector<std::string> {
    auto danglingImpls =  c10::Dispatcher::singleton().findDanglingImpls();

    std::vector<std::string> states;
    states.reserve(danglingImpls.size());
    for (auto& danglingImpl : danglingImpls) {
      states.push_back(danglingImpl.dumpState());
    }

    return states;
  });

  // Prints out the name of every operator that has a kernel registered to the Dispatcher
  // under [dispatch_key].
  // If no arguments are specified, it'll print out the name of every operator that the Dispatcher knows of.
  // This can be useful to answer questions like "list all operators that do not have a CPU kernel".
  m.def("_dispatch_print_registrations_for_dispatch_key", [](const char* dispatch_key = "") {
    auto k = std::string(dispatch_key) == "" ? c10::nullopt : c10::make_optional(c10::parseDispatchKey(dispatch_key));
    auto op_names = c10::Dispatcher::singleton().getRegistrationsForDispatchKey(k);
    for (auto& op : op_names) {
        std::cout << op << std::endl;
    }
  }, py::arg("dispatch_key") = static_cast<const char*>(""));
}

}}} // namespace torch::impl::dispatch
