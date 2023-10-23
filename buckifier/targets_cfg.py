# Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
from __future__ import absolute_import, division, print_function, unicode_literals

rocksdb_target_header_template = """# This file \100generated by:
#$ python3 buckifier/buckify_rocksdb.py{extra_argv}
# --> DO NOT EDIT MANUALLY <--
# This file is a Facebook-specific integration for buck builds, so can
# only be validated by Facebook employees.
load("//rocks/buckifier:defs.bzl", "cpp_library_wrapper","rocks_cpp_library_wrapper","cpp_binary_wrapper","cpp_unittest_wrapper","fancy_bench_wrapper","add_c_test_wrapper")

"""


library_template = """
cpp_library_wrapper(name="{name}", srcs=[{srcs}], deps=[{deps}], headers={headers}, link_whole={link_whole}, extra_test_libs={extra_test_libs})
"""

rocksdb_library_template = """
rocks_cpp_library_wrapper(name="{name}", srcs=[{srcs}], headers={headers})

"""


binary_template = """
cpp_binary_wrapper(name="{name}", srcs=[{srcs}], deps=[{deps}], extra_preprocessor_flags=[{extra_preprocessor_flags}], extra_bench_libs={extra_bench_libs})
"""

unittests_template = """
cpp_unittest_wrapper(name="{test_name}",
            srcs=["{test_cc}"],
            deps={deps},
            extra_compiler_flags={extra_compiler_flags})

"""

fancy_bench_template = """
fancy_bench_wrapper(suite_name="{name}", binary_to_bench_to_metric_list_map={bench_config}, slow={slow}, expected_runtime={expected_runtime}, sl_iterations={sl_iterations}, regression_threshold={regression_threshold})

"""

export_file_template = """
export_file(name = "{name}")
"""
