package(default_visibility = ["//riegeli:__subpackages__"])

licenses(["notice"])  # Apache 2.0

cc_library(
    name = "utils",
    srcs = ["utils.cc"],
    hdrs = ["utils.h"],
    data = [":riegeli_error"],  # Python module imported from C++.
    deps = [
        "//riegeli/base",
        "//riegeli/base:chain",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/meta:type_traits",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/utility",
        "@local_config_python//:python_headers",
    ],
)

py_library(
    name = "riegeli_error",
    srcs = ["riegeli_error.py"],
)
