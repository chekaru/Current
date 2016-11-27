/*******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Dmitry "Dima" Korolev <dmitry.korolev@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * *******************************************************************************/

// FnCAS on-the-fly compilation logic.
// FNCAS_JIT must be defined to enable, the supported values are 'NASM' and 'CLANG'.

#ifndef FNCAS_JIT_H
#define FNCAS_JIT_H

#ifndef FNCAS_USE_LONG_DOUBLE

// Do include this header in the `make test` target for checking there are no leaked symbols.
#ifdef CURRENT_MAKE_CHECK_MODE
#ifndef FNCAS_JIT
#define FNCAS_JIT CLANG
#endif  // FNCAS_JIT
#endif  // CURRENT_MAKE_CHECK_MODE

#ifdef FNCAS_JIT

#include <sstream>
#include <stack>
#include <string>
#include <vector>
#include <iostream>

#include <dlfcn.h>

#include "../../Bricks/strings/printf.h"
#include "../../Bricks/file/file.h"

#include "base.h"
#include "node.h"
#include "differentiate.h"

namespace fncas {

static_assert(std::is_same<double, double_t>::value, "FnCAS JIT assumes `double_t` is the native double.");

// Linux-friendly code to compile into .so and link against it at runtime.
// Not portable.

inline const char* operation_as_nasm_instruction(operation_t operation) {
  static const char* representation[static_cast<size_t>(operation_t::end)] = {
      "addpd", "subpd", "mulpd", "divpd",
  };
  return operation < operation_t::end ? representation[static_cast<size_t>(operation)] : "?";
}

struct compiled_expression final : noncopyable {
  typedef long long (*DIM)();
  typedef long long (*HEAP_SIZE)();
  typedef double (*FUNCTION)(const double* x, double* a);
  typedef void (*GRADIENT)(const double* x, double* a);

  void* lib_;
  DIM dim_;
  HEAP_SIZE heap_size_;
  FUNCTION function_;
  GRADIENT gradient_;

  const std::string lib_filename_;

  using gradient_indexes_t = std::vector<node_index_type>;
  gradient_indexes_t gradient_indexes_;

  explicit compiled_expression(const std::string& lib_filename,
                               const gradient_indexes_t& gradient_indexes = gradient_indexes_t())
      : lib_filename_(lib_filename), gradient_indexes_(gradient_indexes) {
    lib_ = dlopen(lib_filename.c_str(), RTLD_LAZY);
    CURRENT_ASSERT(lib_);
    dim_ = reinterpret_cast<HEAP_SIZE>(dlsym(lib_, "dim"));
    heap_size_ = reinterpret_cast<HEAP_SIZE>(dlsym(lib_, "heap_size"));
    function_ = reinterpret_cast<FUNCTION>(dlsym(lib_, "eval_f"));
    gradient_ = reinterpret_cast<GRADIENT>(dlsym(lib_, "eval_g"));
    CURRENT_ASSERT(dim_);
    CURRENT_ASSERT(heap_size_);
  }

  ~compiled_expression() {
    if (lib_) {
      dlclose(lib_);
    }
  }

  bool HasFunction() const { return function_; }
  bool HasGradient() const { return gradient_; }

  compiled_expression(const compiled_expression&) = delete;
  void operator=(const compiled_expression&) = delete;

  compiled_expression(compiled_expression&& rhs)
      : lib_(std::move(rhs.lib_)),
        dim_(std::move(rhs.dim_)),
        heap_size_(std::move(rhs.heap_size_)),
        function_(std::move(rhs.function_)),
        gradient_(std::move(rhs.gradient_)),
        lib_filename_(std::move(rhs.lib_filename_)) {
    rhs.lib_ = nullptr;
  }

  double compute_compiled_f(const double* x) const {
    CURRENT_ASSERT(dim_);
    CURRENT_ASSERT(heap_size_);
    CURRENT_ASSERT(function_);

    std::vector<double>& heap = internals_singleton().heap_for_compiled_evaluations_;
    const size_t desired_heap_size = heap_size_();
    if (heap.size() < desired_heap_size) {
      heap.resize(desired_heap_size);
    }

    return function_(x, &heap[0]);
  }

  double compute_compiled_f(const std::vector<double>& x) const { return compute_compiled_f(&x[0]); }

  std::vector<double> compute_compiled_g(const double* x) const {
    CURRENT_ASSERT(dim_);
    CURRENT_ASSERT(heap_size_);
    CURRENT_ASSERT(gradient_);
    const auto dim = static_cast<size_t>(dim_());
    CURRENT_ASSERT(gradient_indexes_.size() == dim);

    std::vector<double>& heap = internals_singleton().heap_for_compiled_evaluations_;
    const size_t desired_heap_size = heap_size_();
    if (heap.size() < desired_heap_size) {
      heap.resize(desired_heap_size);
    }

    gradient_(x, &heap[0]);

    CURRENT_ASSERT(gradient_indexes_.size() == dim);
    std::vector<double> result(dim);
    for (size_t i = 0; i < gradient_indexes_.size(); ++i) {
      CURRENT_ASSERT(static_cast<size_t>(gradient_indexes_[i]) < heap.size());
      result[i] = heap[gradient_indexes_[i]];
    }

    return result;
  }

  std::vector<double> compute_compiled_g(const std::vector<double>& x) const { return compute_compiled_g(&x[0]); }

  node_index_type dim() const { return dim_ ? static_cast<size_t>(dim_()) : 0; }
  size_t heap_size() const { return heap_size_ ? static_cast<size_t>(heap_size_()) : 0; }

  static void syscall(const std::string& command) {
    int retval = system(command.c_str());
    if (retval) {
      std::cerr << command << std::endl
                << retval << std::endl;
      exit(-1);
    }
  }

  const std::string& lib_filename() const { return lib_filename_; }
};

struct compile_impl final {
  class NASM final {
   public:
    NASM(const std::string& filebase, bool has_g) : filebase(filebase), f(fopen((filebase + ".asm").c_str(), "w")) {
      CURRENT_ASSERT(f);

      fprintf(f, "[bits 64]\n");
      fprintf(f, "\n");
      fprintf(f, "global %s, dim, heap_size\n", !has_g ? "eval_f" : "eval_g");
      fprintf(f, "extern sqrt, exp, log, sin, cos, tan, asin, acos, atan\n");
      fprintf(f, "\n");
      fprintf(f, "section .text\n");
      fprintf(f, "\n");
    }

    void compile_eval_f(node_index_type index) {
      fprintf(f, "eval_f:\n");
      fprintf(f, "  push rbp\n");
      fprintf(f, "  mov rbp, rsp\n");
      generate_asm_code_for_node(index);
      fprintf(f, "  ; return a[%lld]\n", static_cast<long long>(index));
      fprintf(f, "  movq xmm0, [rsi+%lld]\n", static_cast<long long>(index) * 8);
      fprintf(f, "  mov rsp, rbp\n");
      fprintf(f, "  pop rbp\n");
      fprintf(f, "  ret\n");
    }

    void compile_eval_g(node_index_type f_index, const std::vector<node_index_type>& g_indexes) {
      CURRENT_ASSERT(g_indexes.size() == internals_singleton().dim_);
      fprintf(f, "eval_g:\n");
      fprintf(f, "  push rbp\n");
      fprintf(f, "  mov rbp, rsp\n");
      generate_asm_code_for_node(f_index);
      for (size_t i = 0; i < g_indexes.size(); ++i) {
        generate_asm_code_for_node(g_indexes[i]);
        fprintf(f, "  ; g[%lld] is a[%lld]\n", static_cast<long long>(i), static_cast<long long>(g_indexes[i]));
      }
      fprintf(f, "  ; return a[%lld]\n", static_cast<long long>(f_index));
      fprintf(f, "  movq xmm0, [rsi+%lld]\n", static_cast<long long>(f_index) * 8);
      fprintf(f, "  mov rsp, rbp\n");
      fprintf(f, "  pop rbp\n");
      fprintf(f, "  ret\n");
    }

    ~NASM() {
      fprintf(f, "\n");
      fprintf(f, "dim:\n");
      fprintf(f, "  push rbp\n");
      fprintf(f, "  mov rbp, rsp\n");
      fprintf(f, "  mov rax, %lld\n", static_cast<long long>(internals_singleton().dim_));
      fprintf(f, "  mov rsp, rbp\n");
      fprintf(f, "  pop rbp\n");
      fprintf(f, "  ret\n");
      fprintf(f, "\n");
      fprintf(f, "heap_size:\n");
      fprintf(f, "  push rbp\n");
      fprintf(f, "  mov rbp, rsp\n");
      fprintf(f, "  mov rax, %lld\n", static_cast<long long>(max_dim + 1));
      fprintf(f, "  mov rsp, rbp\n");
      fprintf(f, "  pop rbp\n");
      fprintf(f, "  ret\n");
      fclose(f);

      const char* compile_cmdline = "nasm -O0 -f elf64 %s.asm -o %s.o";
      const char* link_cmdline = "ld -lm -shared -o %s.so %s.o";

      compiled_expression::syscall(current::strings::Printf(compile_cmdline, filebase.c_str(), filebase.c_str()));
      compiled_expression::syscall(current::strings::Printf(link_cmdline, filebase.c_str(), filebase.c_str()));
    }

   private:
    const std::string& filebase;
    FILE* f;
    std::vector<bool> computed;
    node_index_type max_dim = 0;

    // generate_asm_code_for_node() writes NASM code to evaluate the expression to the file.
    void generate_asm_code_for_node(node_index_type index) {
      const double d_0 = 0;
      const double d_1 = 1;
      std::stack<node_index_type> stack;
      stack.push(index);
      while (!stack.empty()) {
        const node_index_type i = stack.top();
        stack.pop();
        const node_index_type dependent_i = ~i;
        if (i > dependent_i) {
          max_dim = std::max(max_dim, static_cast<node_index_type>(i));
          if (computed.size() <= static_cast<size_t>(i)) {
            computed.resize(static_cast<size_t>(i) + 1);
          }
          if (!computed[i]) {
            computed[i] = true;
            node_impl& node = node_vector_singleton()[i];
            if (node.type() == type_t::variable) {
              int32_t v = node.variable();
              fprintf(f, "  ; a[%lld] = x[%d];\n", static_cast<long long>(i), v);
              fprintf(f, "  mov rax, [rdi+%d]\n", v * 8);
              fprintf(f, "  mov [rsi+%lld], rax\n", static_cast<long long>(i) * 8);
            } else if (node.type() == type_t::value) {
              fprintf(f, "  ; a[%lld] = %lf\n", static_cast<long long>(i), node.value());
              fprintf(f, "  mov rax, %s\n", std::to_string(*reinterpret_cast<int64_t*>(&node.value())).c_str());
              fprintf(f, "  mov [rsi+%lld], rax\n", static_cast<long long>(i) * 8);
            } else if (node.type() == type_t::operation) {
              stack.push(~i);
              stack.push(node.lhs_index());
              stack.push(node.rhs_index());
            } else if (node.type() == type_t::function) {
              stack.push(~i);
              stack.push(node.argument_index());
            } else {
              CURRENT_ASSERT(false);
            }
          }
        } else {
          node_impl& node = node_vector_singleton()[dependent_i];
          if (node.type() == type_t::operation) {
            fprintf(f,
                    "  ; a[%lld] = a[%lld] %s a[%lld];\n",
                    static_cast<long long>(dependent_i),
                    static_cast<long long>(node.lhs_index()),
                    operation_as_string(node.operation()),
                    static_cast<long long>(node.rhs_index()));
            fprintf(f, "  movq xmm0, [rsi+%lld]\n", static_cast<long long>(node.lhs_index()) * 8);
            fprintf(f, "  movq xmm1, [rsi+%lld]\n", static_cast<long long>(node.rhs_index()) * 8);
            fprintf(f, "  %s xmm0, xmm1\n", operation_as_nasm_instruction(node.operation()));
            fprintf(f, "  movq [rsi+%lld], xmm0\n", static_cast<long long>(dependent_i) * 8);
          } else if (node.type() == type_t::function) {
            if (node.function() == function_t::sqr) {
              fprintf(f,
                      "  ; a[%lld] = sqr(a[%lld]);  # `sqr` is a special case.\n",
                      static_cast<long long>(dependent_i),
                      static_cast<long long>(node.argument_index()));
              fprintf(f, "  movq xmm0, [rsi+%lld]\n", static_cast<long long>(node.argument_index()) * 8);
              fprintf(f, "  mulpd xmm0, xmm0\n");
              fprintf(f, "  movq [rsi+%lld], xmm0\n", static_cast<long long>(dependent_i) * 8);
            } else if (node.function() == function_t::unit_step) {
              fprintf(f,
                      "  ; a[%lld] = unit_step(a[%lld]);  # `unit_step` is a special case.\n",
                      static_cast<long long>(dependent_i),
                      static_cast<long long>(node.argument_index()));
              fprintf(f, "  movq xmm0, [rsi+%lld]\n", static_cast<long long>(node.argument_index()) * 8);
              fprintf(f, "  mov rax, %s  ; 0\n", std::to_string(*reinterpret_cast<const int64_t*>(&d_0)).c_str());
              fprintf(f, "  movq xmm1, rax\n");
              fprintf(f, "  ucomisd xmm0, xmm1\n");
              fprintf(f, "  jb unit_step_%lld\n", static_cast<long long>(dependent_i));
              fprintf(f, "  mov rax, %s  ; 1\n", std::to_string(*reinterpret_cast<const int64_t*>(&d_1)).c_str());
              fprintf(f, "unit_step_%lld:\n", static_cast<long long>(dependent_i));
              fprintf(f, "  mov [rsi+%lld], rax\n", static_cast<long long>(dependent_i) * 8);
            } else if (node.function() == function_t::ramp) {
              fprintf(f,
                      "  ; a[%lld] = ramp(a[%lld]);  # `ramp` is a special case.\n",
                      static_cast<long long>(dependent_i),
                      static_cast<long long>(node.argument_index()));
              fprintf(f, "  movq xmm0, [rsi+%lld]\n", static_cast<long long>(node.argument_index()) * 8);
              fprintf(f, "  mov rax, %s  ; 0\n", std::to_string(*reinterpret_cast<const int64_t*>(&d_0)).c_str());
              fprintf(f, "  movq xmm1, rax\n");
              fprintf(f, "  ucomisd xmm0, xmm1\n");
              fprintf(f, "  ja ramp_%lld\n", static_cast<long long>(dependent_i));
              fprintf(f, "  movq xmm0, rax\n");
              fprintf(f, "ramp_%lld:\n", static_cast<long long>(dependent_i));
              fprintf(f, "  movq [rsi+%lld], xmm0\n", static_cast<long long>(dependent_i) * 8);
            } else {
              fprintf(f,
                      "  ; a[%lld] = %s(a[%lld]);\n",
                      static_cast<long long>(dependent_i),
                      function_as_string(node.function()),
                      static_cast<long long>(node.argument_index()));
              fprintf(f, "  movq xmm0, [rsi+%lld]\n", static_cast<long long>(node.argument_index()) * 8);
              fprintf(f, "  push rdi\n");
              fprintf(f, "  push rsi\n");
              fprintf(f, "  call %s wrt ..plt\n", function_as_string(node.function()));
              fprintf(f, "  pop rsi\n");
              fprintf(f, "  pop rdi\n");
              fprintf(f, "  movq [rsi+%lld], xmm0\n", static_cast<long long>(dependent_i) * 8);
            }
          } else {
            CURRENT_ASSERT(false);
          }
        }
      }
    }
  };

  class CLANG final {
   public:
    CLANG(const std::string& filebase, bool has_g) : filebase(filebase), f(fopen((filebase + ".c").c_str(), "w")) {
      static_cast<void>(has_g);
      CURRENT_ASSERT(f);
      fprintf(f, "#include <math.h>\n");
      fprintf(f, "#define sqr(x) ((x) * (x))\n");
      fprintf(f, "#define unit_step(x) ((x) >= 0 ? 1 : 0)\n");
      fprintf(f, "#define ramp(x) ((x) >= 0 ? (x) : 0)\n");
    }

    void compile_eval_f(node_index_type index) {
      fprintf(f, "double eval_f(const double* x, double* a) {\n");
      generate_c_code_for_node(index);
      fprintf(f, "  return a[%lld];\n", static_cast<long long>(index));
      fprintf(f, "}\n");
    }

    void compile_eval_g(node_index_type f_index, const std::vector<node_index_type>& g_indexes) {
      CURRENT_ASSERT(g_indexes.size() == internals_singleton().dim_);
      fprintf(f, "double eval_g(const double* x, double* a) {\n");
      generate_c_code_for_node(f_index);
      for (size_t i = 0; i < g_indexes.size(); ++i) {
        generate_c_code_for_node(g_indexes[i]);
      }
      for (size_t i = 0; i < g_indexes.size(); ++i) {
        fprintf(f, "  // g[%lld] is a[%lld]\n", static_cast<long long>(i), static_cast<long long>(g_indexes[i]));
      }
      fprintf(f, "  return a[%lld];\n", static_cast<long long>(f_index));
      fprintf(f, "}\n");
    }

    ~CLANG() {
      fprintf(f, "long long dim() { return %lld; }\n", static_cast<long long>(internals_singleton().dim_));
      fprintf(f, "long long heap_size() { return %lld; }\n", static_cast<long long>(max_dim + 1));

      fclose(f);

      const char* compile_cmdline = "clang -fPIC -shared -nostartfiles %s.c -o %s.so";  // `-O3` is too slow.
      std::string cmdline = current::strings::Printf(compile_cmdline, filebase.c_str(), filebase.c_str());
      compiled_expression::syscall(cmdline);
    }

   private:
    const std::string& filebase;
    FILE* f;
    std::vector<bool> computed;
    node_index_type max_dim = 0;

    // generate_c_code_for_node() writes C code to evaluate the expression to the file.
    void generate_c_code_for_node(node_index_type index) {
      std::stack<node_index_type> stack;
      stack.push(index);
      while (!stack.empty()) {
        const node_index_type i = stack.top();
        stack.pop();
        const node_index_type dependent_i = ~i;
        if (i > dependent_i) {
          max_dim = std::max(max_dim, static_cast<node_index_type>(i));
          if (computed.size() <= static_cast<size_t>(i)) {
            computed.resize(static_cast<size_t>(i) + 1);
          }
          if (!computed[i]) {
            computed[i] = true;
            node_impl& node = node_vector_singleton()[i];
            if (node.type() == type_t::variable) {
              int32_t v = node.variable();
              fprintf(f, "  a[%lld] = x[%d];\n", static_cast<long long>(i), v);
            } else if (node.type() == type_t::value) {
              fprintf(f,
                      "  a[%lld] = %a;  // %lf\n",  // "%a" is hexadecimal full precision.
                      static_cast<long long>(i),
                      node.value(),
                      node.value());  // "%a" is hexadecimal full precision.
            } else if (node.type() == type_t::operation) {
              stack.push(~i);
              stack.push(node.lhs_index());
              stack.push(node.rhs_index());
            } else if (node.type() == type_t::function) {
              stack.push(~i);
              stack.push(node.argument_index());
            } else {
              CURRENT_ASSERT(false);
            }
          }
        } else {
          node_impl& node = node_vector_singleton()[dependent_i];
          if (node.type() == type_t::operation) {
            fprintf(f,
                    "  a[%lld] = a[%lld] %s a[%lld];\n",
                    static_cast<long long>(dependent_i),
                    static_cast<long long>(node.lhs_index()),
                    operation_as_string(node.operation()),
                    static_cast<long long>(node.rhs_index()));
          } else if (node.type() == type_t::function) {
            fprintf(f,
                    "  a[%lld] = %s(a[%lld]);\n",
                    static_cast<long long>(dependent_i),
                    function_as_string(node.function()),
                    static_cast<long long>(node.argument_index()));
          } else {
            CURRENT_ASSERT(false);
          }
        }
      }
    }
  };

  // Confirm the symbol `#define`-d as `FNCAS_JIT` is a valid identifier.
  struct _TMP {
    struct FNCAS_JIT {};
  };
  typedef FNCAS_JIT selected;
};

inline compiled_expression compile_eval_f(node_index_type index) {
  const std::string filebase(current::FileSystem::GenTmpFileName());
  const std::string filename_so = filebase + ".so";
  current::FileSystem::RmFile(filename_so, current::FileSystem::RmFileParameters::Silent);
  {
    compile_impl::selected code_generator(filebase, false);
    code_generator.compile_eval_f(index);
  }
  return compiled_expression(filename_so);
}

inline compiled_expression compile_eval_f(const V& node) { return compile_eval_f(node.index_); }

inline compiled_expression compile_eval_g(node_index_type f_index, const std::vector<node_index_type>& g_indexes) {
  const std::string filebase(current::FileSystem::GenTmpFileName());
  const std::string filename_so = filebase + ".so";
  current::FileSystem::RmFile(filename_so, current::FileSystem::RmFileParameters::Silent);
  {
    compile_impl::selected code_generator(filebase, true);
    code_generator.compile_eval_g(f_index, g_indexes);
  }
  return compiled_expression(filename_so, g_indexes);
}

inline compiled_expression compile_eval_g(const V& f_node, const std::vector<V>& g_nodes) {
  std::vector<node_index_type> g_node_indexes;
  g_node_indexes.reserve(g_nodes.size());
  for (const auto& gi : g_nodes) {
    g_node_indexes.push_back(gi.index_);
  }
  return compile_eval_g(f_node.index_, g_node_indexes);
}

struct f_compiled final : f {
  fncas::compiled_expression c_;

  explicit f_compiled(const V& node) : c_(compile_eval_f(node)) { CURRENT_ASSERT(c_.HasFunction()); }
  explicit f_compiled(const f_intermediate& f) : c_(compile_eval_f(f.f_)) { CURRENT_ASSERT(c_.HasFunction()); }

  f_compiled(const f_compiled&) = delete;
  void operator=(const f_compiled&) = delete;
  f_compiled(f_compiled&& rhs) : c_(std::move(rhs.c_)) { CURRENT_ASSERT(c_.HasFunction()); }

  double operator()(const std::vector<double>& x) const override { return c_.compute_compiled_f(x); }

  size_t dim() const override { return c_.dim(); }
  size_t heap_size() const override { return c_.heap_size(); }

  const std::string& lib_filename() const { return c_.lib_filename(); }
};

struct g_compiled final : g {
  fncas::compiled_expression c_;

  explicit g_compiled(const f_intermediate& f, const g_intermediate& g) : c_(compile_eval_g(f.f_, g.g_)) {
    CURRENT_ASSERT(c_.HasGradient());
  }

  g_compiled(const g_compiled&) = delete;
  void operator=(const g_compiled&) = delete;
  g_compiled(g_compiled&& rhs) : c_(std::move(rhs.c_)) { CURRENT_ASSERT(c_.HasGradient()); }

  std::vector<double_t> operator()(const std::vector<double_t>& x) const override { return c_.compute_compiled_g(x); }

  size_t dim() const override { return c_.dim(); }
  size_t heap_size() const override { return c_.heap_size(); }

  const std::string& lib_filename() const { return c_.lib_filename(); }
};

}  // namespace fncas

#endif  // #ifndef FNCAS_USE_LONG_DOUBLE

#endif  // #ifdef FNCAS_JIT

#endif  // #ifndef FNCAS_JIT_H
