/*
   +----------------------------------------------------------------------+
   | Zend OPcache JIT                                                     |
   +----------------------------------------------------------------------+
   | Copyright (c) 1998-2014 The PHP Group                                |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Authors: Dmitry Stogov <dmitry@zend.com>                             |
   |          Xinchen Hui <laruence@php.net>                              |
   +----------------------------------------------------------------------+
*/

/* $Id:$ */

#include "main/php.h"
#include <ZendAccelerator.h>
#include "Zend/zend_generators.h"

#include "jit/zend_jit_config.h"
#include "jit/zend_jit_context.h"
#include "jit/zend_jit_codegen.h"
#include "jit/zend_jit_helpers.h"
#include "jit/zend_worklist.h"

#define ZEND_LLVM_DEBUG                0x0303

#define ZEND_LLVM_DEBUG_VERIFY_IR      0x0001
#define ZEND_LLVM_DEBUG_DUMP           0x0002

#define ZEND_LLVM_DEBUG_CODEGEN        0x0010

#define ZEND_LLVM_DEBUG_SYMBOLS        0x0100
#define ZEND_LLVM_DEBUG_GDB            0x0200

#define ZEND_LLVM_MODULE_AT_ONCE       1

#if ZEND_LLVM_DEBUG & ZEND_LLVM_DEBUG_SYMBOLS
# ifndef _GNU_SOURCE
#  define _GNU_SOURCE
# endif
# include <dlfcn.h>
#endif

#if ZEND_LLVM_DEBUG & ZEND_LLVM_DEBUG_SYMBOLS
# define ZEND_JIT_SYM(sym) sym
#else
# define ZEND_JIT_SYM(sym) ""
#endif

#include <stdint.h>

#include "llvm/Config/llvm-config.h"

#if (LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR >= 5 && LLVM_VERSION_MINOR <= 5)
# include "llvm/IR/Module.h"
# include "llvm/IR/IRBuilder.h"
# include "llvm/IR/Intrinsics.h"
# include "llvm/IR/MDBuilder.h"
# include "llvm/IR/Verifier.h"
# include "llvm/Support/Host.h"
#elif (LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR >= 3 && LLVM_VERSION_MINOR <= 4)
# include "llvm/IR/Module.h"
# include "llvm/IR/IRBuilder.h"
# include "llvm/IR/Intrinsics.h"
# include "llvm/IR/MDBuilder.h"
# include "llvm/Analysis/Verifier.h"
#elif (LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR == 2)
# include "llvm/Module.h"
# include "llvm/IRBuilder.h"
# include "llvm/Intrinsics.h"
# include "llvm/MDBuilder.h"
# include "llvm/Analysis/Verifier.h"
#else
# error "Unsupported LLVM version (only versions between 3.2 and 3.5 are supported)"
#endif

#include "llvm/Support/TargetSelect.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/JIT.h"
#include "llvm/ExecutionEngine/JITMemoryManager.h"
#ifdef HAVE_OPROFILE
# include "llvm/ExecutionEngine/JITEventListener.h"
#endif
#if ZEND_LLVM_DEBUG & ZEND_LLVM_DEBUG_GDB
# include "llvm/ExecutionEngine/MCJIT.h"
#endif
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/PassManager.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/DynamicLibrary.h"

#ifdef _WIN32
# include <windows.h>
# include <winbase.h>
#else
# include <unistd.h>
# include <sys/mman.h>
# ifndef MAP_ANONYMOUS
#  ifdef MAP_ANON
#   define MAP_ANONYMOUS MAP_ANON
#  endif
# endif
#endif

#ifndef offsetof
# define offsetof(type, field) ((zend_uintptr_t)(&(((type*)0)->field)))
#endif

#define JIT_CHECK(func) do { \
		if (!(func)) return 0; \
	} while (0)

// Macros to isolate architecture differences in LLVM code generation
// The following are methods of Builder:
#if SIZEOF_ZEND_LONG == 8
# define LLVM_GET_LONG_TY			Type::getInt64Ty
# define LLVM_GET_LONG				llvm_ctx.builder.getInt64
# define LLVM_CREATE_CONST_GEP1		llvm_ctx.builder.CreateConstGEP1_64
# define LLVM_CREATE_CONST_GEP2		llvm_ctx.builder.CreateConstGEP2_64
#else
# define LLVM_GET_LONG_TY			Type::getInt32Ty
# define LLVM_GET_LONG				llvm_ctx.builder.getInt32
# define LLVM_CREATE_CONST_GEP1		llvm_ctx.builder.CreateConstGEP1_32
# define LLVM_CREATE_CONST_GEP2		llvm_ctx.builder.CreateConstGEP2_32
#endif

#define LLVM_GET_CONST_STRING(str)  llvm_ctx.builder.CreateIntToPtr(       \
				LLVM_GET_LONG((zend_uintptr_t)(str)),     \
				PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context))) \

#define PHI_DCL(name, count) \
	Value *name ## _phi_val[count]; \
	BasicBlock *name ## _phi_bb[count]; \
	int name ## _phi_count = 0;	

#define PHI_COUNT(name)	(name ## _phi_count)

#define PHI_DCL2(name, count) \
	Value *name ## _phi_val[count]; \
	BasicBlock *name ## _phi_bb[count]; \
	int name ## _phi_count = 0;	\
	PHINode *name ## _phi = NULL;

#define PHI_ADD(name, val) do { \
		name ## _phi_val[name ## _phi_count] = val; \
		name ## _phi_bb[name ## _phi_count] = llvm_ctx.builder.GetInsertBlock(); \
		name ## _phi_count++; \
	} while (0)

#define PHI_ADD2(name, val) do { \
		name ## _phi_val[name ## _phi_count] = val; \
		name ## _phi_bb[name ## _phi_count] = llvm_ctx.builder.GetInsertBlock(); \
		if (name ## _phi) { \
			name ## _phi->addIncoming(val, llvm_ctx.builder.GetInsertBlock()); \
		} \
		name ## _phi_count++; \
	} while (0)

#define PHI_SET(name, var, type) do { \
		ZEND_ASSERT(name ## _phi_count > 0); \
		if (name ## _phi_count == 1) { \
			var = name ## _phi_val[0]; \
		} else { \
			PHINode *phi = llvm_ctx.builder.CreatePHI(type, name ## _phi_count); \
			int i; \
			for (i = 0; i < name ## _phi_count; i++) { \
				phi->addIncoming(name ## _phi_val[i], name ## _phi_bb[i]); \
			} \
			var = phi; \
		} \
	} while (0)

#define PHI_SET2(name, var, type) do { \
		name ## _phi = llvm_ctx.builder.CreatePHI(type, name ## _phi_count); \
		int i; \
		for (i = 0; i < name ## _phi_count; i++) { \
			name ## _phi->addIncoming(name ## _phi_val[i], name ## _phi_bb[i]); \
		} \
		var = name ## _phi; \
	} while (0)

#define zend_jit_load_obj_handler(llvm_ctx, handlers, handler)                \
		llvm_ctx.builder.CreateAlignedLoad(                                   \
				zend_jit_GEP(                                                 \
					llvm_ctx,                                                 \
					(handlers),                                               \
					offsetof(zend_object_handlers, handler),                  \
					PointerType::getUnqual(                                   \
						PointerType::getUnqual(                               \
							Type::LLVM_GET_LONG_TY(llvm_ctx.context)))), 4)

#define IS_CUSTOM_HANDLERS(scope)   (!(scope) || (scope)->create_object) 

using namespace llvm;

#ifdef __cplusplus
extern "C" {
#endif

#ifdef HAVE_OPROFILE
static JITEventListener *event_listener = NULL; 
#endif

typedef struct _zend_asm_buf {
	zend_uchar *base;
	zend_uchar *ptr;
	zend_uchar *end;
} zend_asm_buf;

static zend_asm_buf *asm_buf = NULL;

#if JIT_STAT
typedef struct _zend_jit_stat {
	long compiled_scripts;
	long compiled_op_arrays;
	long compiled_clones;
	long inlined_clones;
	long ssa_vars;
	long untyped_ssa_vars;
	long typed_ssa_vars;
	long reg_ssa_vars;
} zend_jit_stat;

static zend_jit_stat jit_stat = {0, 0, 0, 0, 0, 0, 0, 0};
#endif

typedef struct _zend_llvm_ctx {
	zend_op_array   *op_array;

	int              inline_level;
	int              call_level;
	zend_bool        valid_opline;

	LLVMContext     &context;
    IRBuilder<>      builder;

	Type            *zval_type;
	Type            *zval_ptr_type;
	Type            *zend_string_type;
	Type            *zend_array_type;
	Type            *zend_object_type;
	Type            *zend_res_type;
	Type            *HashTable_type;
	Type            *zend_execute_data_type;
	Type            *zend_vm_stack_type;
	Type            *zend_constant_type;
	Type            *zend_function_type;
	Type            *zend_op_array_type;
	Type            *zend_class_entry_type;
	Type            *zend_op_type;
	Type            *zend_refcounted_type;
	Type            *zend_property_info_type;
	Type            *zend_arena_type;

    FunctionType    *handler_type;
    FunctionType    *internal_func_type;

    Module          *module;
	Function        *function;
	ExecutionEngine *engine;
	TargetMachine   *target;

    Value           *_execute_data;
	GlobalVariable  *_CG_empty_string;
	GlobalVariable  *_CG_one_char_string;
	GlobalVariable  *_CG_arena;
	GlobalVariable  *_EG_exception;
	GlobalVariable  *_EG_vm_stack_top;
	GlobalVariable  *_EG_vm_stack_end;
	GlobalVariable  *_EG_vm_stack;
	GlobalVariable  *_EG_objects_store;
	GlobalVariable  *_EG_uninitialized_zval;
	GlobalVariable  *_EG_error_zval;
	GlobalVariable  *_EG_current_execute_data;
	GlobalVariable  *_EG_function_table;
	GlobalVariable  *_EG_scope;
	GlobalVariable  *_EG_symbol_table;
	GlobalVariable  *_EG_symtable_cache_ptr;
	GlobalVariable  *_EG_symtable_cache_limit;
	GlobalVariable  *_EG_precision;
	GlobalVariable  *_zend_execute_internal;
	Value           *function_name;

	Value           *stack_slots[32];
	Value          **reg;
//???	Value           *ret_reg;
//???	Value          **arg_reg;
//???	Value          **param_reg;
//???	Value           *param_tmp;
//???	int              param_top;

	BasicBlock     **bb_labels;
	BasicBlock     **bb_exceptions;
	BasicBlock      *bb_exception_exit;
	BasicBlock      *bb_inline_return;
	BasicBlock      *bb_leave;

	HashTable        functions;
	zend_mm_heap    *mm_heap;
	void            *mm_alloc;
	void            *mm_free;

	zend_bitset      this_checked; /* bitset of basic blocks where $this is already checked */

	_zend_llvm_ctx(LLVMContext &_context):
		context(_context),
		builder(context)
	{
		op_array = NULL;

		inline_level = 0;
		call_level = 0;

		valid_opline = 0;

		zval_type = NULL;
		zval_ptr_type = NULL;
		zend_string_type = NULL;
		zend_res_type = NULL;
		HashTable_type = NULL;
		zend_execute_data_type = NULL;
		zend_vm_stack_type = NULL;
		zend_constant_type = NULL;
		zend_function_type = NULL;
		zend_op_array_type = NULL;
		zend_class_entry_type = NULL;
		zend_refcounted_type = NULL;
		zend_object_type = NULL;
		zend_property_info_type = NULL;
		zend_arena_type = NULL;
		handler_type = NULL;
		internal_func_type = NULL;

		module = NULL;
		function = NULL;
		engine = NULL;

		_CG_empty_string = NULL;
		_CG_one_char_string = NULL;
		_CG_arena = NULL;
		_EG_exception = NULL;
		_EG_vm_stack_top = NULL;
		_EG_vm_stack_end = NULL;
		_EG_vm_stack = NULL;
		_EG_objects_store = NULL;
		_EG_uninitialized_zval = NULL;
		_EG_error_zval = NULL;
		_EG_current_execute_data = NULL;
		_EG_function_table = NULL;
		_EG_scope = NULL;
		_EG_symbol_table = NULL;
		_EG_symtable_cache_ptr = NULL;
		_EG_symtable_cache_limit = NULL;
		_EG_precision = NULL;
		_zend_execute_internal = NULL;

		_execute_data = NULL;
		function_name = NULL;

		reg = NULL;
//???		ret_reg = NULL;
//???		arg_reg = NULL;
//???		param_reg = NULL;
//???		param_tmp = NULL;
//???		param_top = 0;
		bb_labels = NULL;
		bb_exceptions = NULL;
		bb_exception_exit = NULL;
		bb_inline_return = NULL;

		mm_heap = NULL;
		mm_alloc = NULL;
		mm_free = NULL;

		this_checked = NULL;

//???		memset(garbage, 0, sizeof(garbage));
	}
} zend_llvm_ctx;

static void (*orig_execute_ex)(zend_execute_data *ex TSRMLS_DC);

static void jit_execute_ex(zend_execute_data *ex TSRMLS_DC) {
	orig_execute_ex(ex TSRMLS_CC);
}

/* JIT Memory Manager */

class ZendJITMemoryManager : public JITMemoryManager {
  friend class ExecutionEngine;
public:
  ExecutionEngine *Engine;
  Module *Mod;

  ZendJITMemoryManager() {
    Engine = NULL;
    Mod = NULL;
  }
  ~ZendJITMemoryManager();

  virtual void setEngine(ExecutionEngine *engine, Module *mod) {
    Engine = engine;
    Mod = mod;
  }

#if (LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR == 2)
  virtual uint8_t *allocateCodeSection(uintptr_t Size, unsigned Alignment,
                                       unsigned SectionID);

  virtual uint8_t *allocateDataSection(uintptr_t Size, unsigned Alignment,
                                       unsigned SectionID);
#elif (LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR == 3)
  virtual uint8_t *allocateCodeSection(uintptr_t Size, unsigned Alignment,
                                       unsigned SectionID);

  virtual uint8_t *allocateDataSection(uintptr_t Size, unsigned Alignment,
                                       unsigned SectionID, bool IsReadOnly);

  virtual bool applyPermissions(std::string *ErrMsg = 0) {
  	return true;
  }
#else
  virtual uint8_t *allocateCodeSection(uintptr_t Size, unsigned Alignment,
                                       unsigned SectionID, StringRef SectionName);

  virtual uint8_t *allocateDataSection(uintptr_t Size, unsigned Alignment,
                                       unsigned SectionID, StringRef SectionName, bool IsReadOnly);

  virtual bool finalizeMemory(std::string *ErrMsg = 0) {
  	return true;
  }
#endif

#if (LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR <= 3)
	virtual void *getPointerToNamedFunction(const std::string &Name,
                                            bool AbortOnFailure);
#else
	virtual uint64_t getSymbolAddress(const std::string &Name);
#endif

  // Invalidate instruction cache for code sections. Some platforms with
  // separate data cache and instruction cache require explicit cache flush,
  // otherwise JIT code manipulations (like resolved relocations) will get to
  // the data cache but not to the instruction cache.
  virtual void invalidateInstructionCache();

  // The RTDyldMemoryManager doesn't use the following functions, so we don't
  // need implement them.
  virtual void setMemoryWritable() {
    llvm_unreachable("Unexpected call!");
  }
  virtual void setMemoryExecutable() {
    llvm_unreachable("Unexpected call!");
  }
  virtual void setPoisonMemory(bool poison) {
    llvm_unreachable("Unexpected call!");
  }
  virtual void AllocateGOT() {
    llvm_unreachable("Unexpected call!");
  }
  virtual uint8_t *getGOTBase() const {
    llvm_unreachable("Unexpected call!");
    return 0;
  }
  virtual uint8_t *startFunctionBody(const Function *F,
                                     uintptr_t &ActualSize){
    llvm_unreachable("Unexpected call!");
    return 0;
  }
  virtual uint8_t *allocateStub(const GlobalValue* F, unsigned StubSize,
                                unsigned Alignment) {
    llvm_unreachable("Unexpected call!");
    return 0;
  }
  virtual void endFunctionBody(const Function *F, uint8_t *FunctionStart,
                               uint8_t *FunctionEnd) {
    llvm_unreachable("Unexpected call!");
  }
  virtual uint8_t *allocateSpace(intptr_t Size, unsigned Alignment) {
    llvm_unreachable("Unexpected call!");
    return 0;
  }
  virtual uint8_t *allocateGlobal(uintptr_t Size, unsigned Alignment) {
    llvm_unreachable("Unexpected call!");
    return 0;
  }
  virtual void deallocateFunctionBody(void *Body) {
    llvm_unreachable("Unexpected call!");
  }
  virtual uint8_t* startExceptionTable(const Function* F,
                                       uintptr_t &ActualSize) {
    llvm_unreachable("Unexpected call!");
    return 0;
  }
  virtual void endExceptionTable(const Function *F, uint8_t *TableStart,
                                 uint8_t *TableEnd, uint8_t* FrameRegister) {
    llvm_unreachable("Unexpected call!");
  }
  virtual void deallocateExceptionTable(void *ET) {
    llvm_unreachable("Unexpected call!");
  }
};

ZendJITMemoryManager::~ZendJITMemoryManager() {
}

#if (LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR == 2)
uint8_t *ZendJITMemoryManager::allocateDataSection(uintptr_t Size,
                                                   unsigned Alignment,
                                                   unsigned SectionID) {
#elif (LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR == 3)
uint8_t *ZendJITMemoryManager::allocateDataSection(uintptr_t Size,
                                                   unsigned  Alignment,
                                                   unsigned  SectionID,
                                                   bool      IsReadOnly) {
#else
uint8_t *ZendJITMemoryManager::allocateDataSection(uintptr_t Size,
                                                   unsigned  Alignment,
                                                   unsigned  SectionID,
                                                   StringRef SectionName,
                                                   bool      IsReadOnly) {
#endif
	if (!Alignment)
		Alignment = 16;

	uint8_t *AlignedAddr = (uint8_t*)RoundUpToAlignment((uint64_t)asm_buf->ptr, Alignment);
	asm_buf->ptr = AlignedAddr + Size;
	if (asm_buf->ptr > asm_buf->end) {
		fprintf(stderr, "JIT BUFFER OVERFLOW\n");
		exit(-1);
	}
	return AlignedAddr;
}

#if (LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR <= 3)
uint8_t *ZendJITMemoryManager::allocateCodeSection(uintptr_t Size,
                                                   unsigned  Alignment,
                                                   unsigned  SectionID) {
#else
uint8_t *ZendJITMemoryManager::allocateCodeSection(uintptr_t Size,
                                                   unsigned  Alignment,
                                                   unsigned  SectionID,
                                                   StringRef SectionName) {
#endif
	if (!Alignment)
		Alignment = 16;

	uint8_t *AlignedAddr = (uint8_t*)RoundUpToAlignment((uint64_t)asm_buf->ptr, Alignment);
	asm_buf->ptr = AlignedAddr + Size;
	if (asm_buf->ptr > asm_buf->end) {
		fprintf(stderr, "JIT BUFFER OVERFLOW\n");
		exit(-1);
	}

	return AlignedAddr;
}

void ZendJITMemoryManager::invalidateInstructionCache() {
  sys::Memory::InvalidateInstructionCache(asm_buf->base,
                                          asm_buf->end - asm_buf->base);
}

#if (LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR <= 3)
void *ZendJITMemoryManager::getPointerToNamedFunction(const std::string &Name,
                                                      bool AbortOnFailure)
#else
uint64_t ZendJITMemoryManager::getSymbolAddress(const std::string &Name)
#endif
{
  // Resolve external symbols with global mapping (FIXME: Ugly LLVM hack)
  if (Mod && Engine) {
    GlobalValue *Val = Mod->getNamedValue(Name);
    if (Val) {
      void *Ptr = Engine->getPointerToGlobalIfAvailable(Val);
#if (LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR <= 3)
      if (Ptr) return Ptr;
#else
      if (Ptr) return (uint64_t)Ptr;
#endif
	}
  }

  const char *NameStr = Name.c_str();
  void *Ptr = sys::DynamicLibrary::SearchForAddressOfSymbol(NameStr);
#if (LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR <= 3)
  if (Ptr) return Ptr;
#else
  if (Ptr) return (uint64_t)Ptr;
#endif

  // If it wasn't found and if it starts with an underscore ('_') character,
  // try again without the underscore.
  if (NameStr[0] == '_') {
    Ptr = sys::DynamicLibrary::SearchForAddressOfSymbol(NameStr+1);
#if (LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR <= 3)
    if (Ptr) return Ptr;
#else
    if (Ptr) return (uint64_t)Ptr;
#endif
  }

#if (LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR <= 3)
  if (AbortOnFailure)
    report_fatal_error("JIT used external function '" + Name +
                      "' which could not be resolved!");
#endif

  return 0;
}

/* bit helpers */

/* from http://aggregate.org/MAGIC/ */
static uint32_t ones32(uint32_t x)
{
	x -= ((x >> 1) & 0x55555555);
	x = (((x >> 2) & 0x33333333) + (x & 0x33333333));
	x = (((x >> 4) + x) & 0x0f0f0f0f);
	x += (x >> 8);
	x += (x >> 16);
	return x & 0x0000003f;
}

static uint32_t floor_log2(uint32_t x)
{
	x |= (x >> 1);
	x |= (x >> 2);
	x |= (x >> 4);
	x |= (x >> 8);
	x |= (x >> 16);
	return ones32(x) - 1;
}

static zend_bool is_power_of_two(uint32_t x)
{
	return !(x & (x - 1));
}

static zend_bool has_concrete_type(uint32_t value_type)
{
	value_type &= MAY_BE_ANY;
	return is_power_of_two (value_type);
}

static zend_bool concrete_type(uint32_t value_type)
{
	return floor_log2(value_type & MAY_BE_ANY);
}

/* Codegenerator */

static int zend_jit_unum(void)
{
	// FIXME: must be unique across shared processes
	static int n = 0;
	n++;
	return n;
}

#if ZEND_DEBUG
/* {{{ static Value* zend_jit_function_name */
static Value* zend_jit_function_name(zend_llvm_ctx &llvm_ctx)
{
	if (!llvm_ctx.function_name) {
		llvm_ctx.function_name = llvm_ctx.builder.CreateGlobalStringPtr(llvm_ctx.function->getName());
	}
	return llvm_ctx.function_name;
}
/* }}} */
#endif

/* {{{ static const char* zend_jit_func_name */
static const char* zend_jit_func_name(zend_jit_context   *ctx,
                                      zend_op_array      *op_array,
                                      zend_jit_func_info *info)
{
	char str[2048];
	int i;
	int len = 0;

	if (ZEND_ACC_CLOSURE & op_array->fn_flags) {
		len = snprintf(str, 2048, "ZEND_JIT__closure__%d", zend_jit_unum());
 	} else if (op_array->function_name) {
		if (op_array->scope && op_array->scope->name) {
			len = snprintf(str, 2048, "ZEND_JIT__%s__%s", op_array->scope->name->val, op_array->function_name->val);
		} else {
			len = snprintf(str, 2048, "ZEND_JIT__%s", op_array->function_name->val);
		}
		for (i = 0; i < len; i++) {
			if (str[i] == '\\') {
				str[i] = '_';
			}
		}
	} else {
		len = snprintf(str, 2048, "ZEND_JIT__main__%d", zend_jit_unum());
	}
	if (info->clone_num > 0) {
		len += snprintf(str + len, 2048 - len, "__clone_%d", info->clone_num);
	}
	char *ret = (char*)zend_arena_alloc(&ctx->arena, len+1);
	memcpy(ret, str, len+1);
	return ret;
}
/* }}} */

/* {{{ static Function* zend_jit_get_func */
static Function* zend_jit_get_func(zend_llvm_ctx      &llvm_ctx,
                                   zend_jit_context   *ctx,
                                   zend_op_array      *op_array,
                                   zend_jit_func_info *info)
{
	if (info->codegen_data) {
		return (Function*)info->codegen_data;
	} else {
	    const char *name = zend_jit_func_name(ctx, op_array, info);
		std::vector<llvm::Type *> args;
		FunctionType *type;
		Type *return_type;
		int num_args = 0;

		if (info->clone_num) {
			if (info->return_info.type & MAY_BE_IN_REG) {
				if (info->return_info.type & MAY_BE_DOUBLE) {
					return_type = Type::getDoubleTy(llvm_ctx.context);
				} else if (info->return_info.type & (MAY_BE_LONG|MAY_BE_FALSE|MAY_BE_TRUE)) {
					return_type = LLVM_GET_LONG_TY(llvm_ctx.context);
				} else {
					ASSERT_NOT_REACHED();
				}
			} else {
				return_type = Type::getVoidTy(llvm_ctx.context);
			}
		} else {
			return_type = Type::getInt32Ty(llvm_ctx.context);
		}
		if (!(info->flags & ZEND_JIT_FUNC_NO_FRAME)) {
			args.push_back(PointerType::getUnqual(llvm_ctx.zend_execute_data_type));
			num_args++;
		}
		if (info->flags & ZEND_JIT_FUNC_HAS_REG_ARGS) {
			int i;

			for (i = 0; i < info->num_args; i++) {
				if (info->arg_info[i].info.type & MAY_BE_IN_REG) {
					if (info->arg_info[i].info.type & (MAY_BE_LONG|MAY_BE_FALSE|MAY_BE_TRUE)) {
						args.push_back(LLVM_GET_LONG_TY(llvm_ctx.context));
					} else if (info->arg_info[i].info.type & (MAY_BE_DOUBLE)) {
						args.push_back(Type::getDoubleTy(llvm_ctx.context));
					} else {
						ASSERT_NOT_REACHED();
					}
					num_args++;
//???				} else if (info->arg_info[i].info.type & MAY_BE_TMP_ZVAL) {
//???					args.push_back(llvm_ctx.zval_ptr_type);
//???					num_args++;
				} 
			}
		} 
//???		if (info->return_info.type & MAY_BE_TMP_ZVAL) {
//???			args.push_back(llvm_ctx.zval_ptr_type);
//???			num_args++;
//???		}
		type = FunctionType::get(
			return_type,
			ArrayRef<Type*>(args),
			false);
		Function *func = Function::Create(
			type,
			Function::ExternalLinkage,
			name,
			llvm_ctx.module);
		func->setCallingConv(CallingConv::X86_FastCall);
		if (num_args >= 1) {
#if (LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR == 2)
			func->addAttribute(1,
				Attributes::get(llvm_ctx.context, Attributes::InReg));
#else
			func->addAttribute(1, Attribute::InReg);
#endif
		}
		if (num_args >= 2) {
#if (LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR == 2)
			func->addAttribute(2,
				Attributes::get(llvm_ctx.context, Attributes::InReg));
#else
			func->addAttribute(2, Attribute::InReg);
#endif
		}
		info->codegen_data = func;
		return func;
	}
}
/* }}} */

/* ??? ... ??? */

/* {{{ static void zend_jit_expected_br */
static void zend_jit_expected_br(zend_llvm_ctx &llvm_ctx,
                                 Value         *cmp,
                                 BasicBlock    *bb_true,
                                 BasicBlock    *bb_false,
                                 int            cost_true = 64,
                                 int            cost_false = 4)
{
#if 0
	llvm_ctx.builder.CreateCondBr(
		llvm_ctx.builder.CreateICmpNE(
			llvm_ctx.builder.CreateCall2(
				Intrinsic::getDeclaration(llvm_ctx.module, Intrinsic::expect, ArrayRef<Type*>(Type::getInt1Ty(llvm_ctx.context))),
				cmp,
				llvm_ctx.builder.getInt1(1)),
			llvm_ctx.builder.getInt1(0)),
		bb_true,
		bb_false);
#else
	Instruction *br = llvm_ctx.builder.CreateCondBr(
		cmp,
		bb_true,
		bb_false);
	MDBuilder MDB(br->getContext());
	br->setMetadata(LLVMContext::MD_prof, MDB.createBranchWeights(cost_true, cost_false));
#endif
}
/* }}} */

/* {{{ static void zend_jit_unexpected_br */
static void zend_jit_unexpected_br(zend_llvm_ctx &llvm_ctx,
                                   Value         *cmp,
                                   BasicBlock    *bb_true,
                                   BasicBlock    *bb_false,
                                   int            cost_true = 4,
                                   int            cost_false = 64)
{
#if 0
	llvm_ctx.builder.CreateCondBr(
		llvm_ctx.builder.CreateICmpNE(
			llvm_ctx.builder.CreateCall2(
				Intrinsic::getDeclaration(llvm_ctx.module, Intrinsic::expect, ArrayRef<Type*>(Type::getInt1Ty(llvm_ctx.context))),
				cmp,
				llvm_ctx.builder.getInt1(0)),
			llvm_ctx.builder.getInt1(0)),
		bb_true,
		bb_false);
#else
	Instruction *br = llvm_ctx.builder.CreateCondBr(
		cmp,
		bb_true,
		bb_false);
	MDBuilder MDB(br->getContext());
	br->setMetadata(LLVMContext::MD_prof, MDB.createBranchWeights(cost_true, cost_false));
#endif
}
/* }}} */

/* {{{ static void zend_jit_expected_br_ex */
static void zend_jit_expected_br_ex(zend_llvm_ctx &llvm_ctx,
                                    Value         *cmp,
                                    BasicBlock    *bb_true,
                                    BasicBlock    *bb_false,
                                    int            expected_branch)
{
	if (expected_branch < 0) {
		llvm_ctx.builder.CreateCondBr(
			cmp,
			bb_true,
			bb_false);
	} else if (expected_branch) {
		zend_jit_expected_br(llvm_ctx, cmp, bb_true, bb_false);
	} else {
		zend_jit_unexpected_br(llvm_ctx, cmp, bb_true, bb_false);
	}
}
/* }}} */

/* {{{ static inline Value* zend_jit_GEP */
static inline Value* zend_jit_GEP(zend_llvm_ctx &llvm_ctx,
                                  Value         *base,
                                  long           offset,
                                  Type          *type)
{
	if (offset == 0) {
		return llvm_ctx.builder.CreateBitCast(
				base,
				type);
	}
	Type *ty = base->getType();
	if (ty->isPointerTy()) {
		ty = ty->getPointerElementType();
		if (ty->isArrayTy()) {
			ty = ty->getArrayElementType();
			if (ty->isSized()) {
				size_t base_element_size = llvm_ctx.engine->getDataLayout()->getTypeAllocSize(ty);
				if (offset % base_element_size == 0) {
					// FIXME Intel PHP: Make sure we don't lose high-order bits of negative offsets in 64-bit.
					long elem_offset = offset / (long)base_element_size;
					return llvm_ctx.builder.CreateBitCast(
							LLVM_CREATE_CONST_GEP2(
								base,
								0,
								elem_offset),
							type);
				}
	    	}
		} else {
			if (ty->isSized()) {
				size_t base_element_size = llvm_ctx.engine->getDataLayout()->getTypeAllocSize(ty);
				if (offset % base_element_size == 0) {
					// FIXME Intel PHP: Make sure we don't lose high-order bits of negative offsets in 64-bit.
					long elem_offset = offset / (long)base_element_size;
					return llvm_ctx.builder.CreateBitCast(
							LLVM_CREATE_CONST_GEP1(
								base,
								elem_offset),
							type);
				}
	    	}
		}
	}
	return llvm_ctx.builder.CreateBitCast(
			LLVM_CREATE_CONST_GEP1(
				llvm_ctx.builder.CreateBitCast(
					base,
					PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context))),
				offset),
			type);
}
/* }}} */

/* {{{ static int zend_jit_call_handler */
static int zend_jit_call_handler(zend_llvm_ctx &llvm_ctx,
                                 zend_op       *opline,
                                 bool           tail_call)
{
	Function *_handler = const_cast<Function*>(cast_or_null<Function>(llvm_ctx.engine->getGlobalValueAtAddress((void*)opline->handler)));
	if (!_handler) {
#if ZEND_LLVM_DEBUG & ZEND_LLVM_DEBUG_SYMBOLS
		Dl_info info;
		if (dladdr((void*)(zend_uintptr_t)opline->handler, &info) &&
		    info.dli_sname != NULL &&
	    	info.dli_saddr == (void*)(zend_uintptr_t)opline->handler) {
			_handler = Function::Create(
				llvm_ctx.handler_type,
				Function::ExternalLinkage,
				info.dli_sname,
				llvm_ctx.module);
		} else {
			typedef struct _zend_jit_op_type_desc {
				const char *name;
			} zend_jit_op_type_desc;
			static const zend_jit_op_type_desc op_type[] = {
				{""},
				{"_CONST"},
				{"_TMP"},
				{""},
				{"_VAR"},
				{""},
				{""},
				{""},
				{"_UNUSED"},
				{""},
				{""},
				{""},
				{""},
				{""},
				{""},
				{""},
				{"_CV"}
			};
			static const zend_jit_op_type_desc op_type2[] = {
				{""},
				{"_CONST"},
				{"_TMPVAR"},
				{""},
				{"_TMPVAR"},
				{""},
				{""},
				{""},
				{"_UNUSED"},
				{""},
				{""},
				{""},
				{""},
				{""},
				{""},
				{""},
				{"_CV"}
			};
			const char *name = zend_get_opcode_name(opline->opcode);
			uint32_t flags = zend_get_opcode_flags(opline->opcode);
			_handler = Function::Create(
				llvm_ctx.handler_type,
				Function::ExternalLinkage,
				Twine("ZEND_") + (name+5) + "_SPEC" +
					((flags & ZEND_VM_OP1_SPEC) ? 
						((flags & ZEND_VM_OP1_TMPVAR) ? 
							op_type2[OP1_OP_TYPE()].name : op_type[OP1_OP_TYPE()].name) : "") +
					((flags & ZEND_VM_OP2_SPEC) ?
						((flags & ZEND_VM_OP2_TMPVAR) ? 
							op_type2[OP2_OP_TYPE()].name : op_type[OP2_OP_TYPE()].name) : "") +
 					"_HANDLER",
				llvm_ctx.module);
		}
#else
		_handler = Function::Create(
			llvm_ctx.handler_type,
			Function::ExternalLinkage,
			"",
			llvm_ctx.module);
#endif

		_handler->setCallingConv(CallingConv::X86_FastCall);
#if (LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR == 2)
		_handler->addAttribute(1,
			Attributes::get(llvm_ctx.context, Attributes::InReg));
#else
		_handler->addAttribute(1, Attribute::InReg);
#endif
		llvm_ctx.engine->addGlobalMapping(_handler, (void*)opline->handler);
	}

	CallInst *call = llvm_ctx.builder.CreateCall(_handler, llvm_ctx._execute_data);
	call->setCallingConv(CallingConv::X86_FastCall);
	if (tail_call) {
		if (llvm_ctx.inline_level) {
			if (!llvm_ctx.bb_inline_return) {
				llvm_ctx.bb_inline_return = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(llvm_ctx.bb_inline_return);
		} else {
			zend_jit_func_info *info = JIT_DATA(llvm_ctx.op_array);
			if (info->clone_num) {
				if (info->return_info.type & MAY_BE_IN_REG) {
					if (info->return_info.type & MAY_BE_DOUBLE) {
						llvm_ctx.builder.CreateRet(ConstantFP::get(Type::getDoubleTy(llvm_ctx.context), 0.0));
					} else if (info->return_info.type & (MAY_BE_LONG|MAY_BE_FALSE|MAY_BE_TRUE)) {
						llvm_ctx.builder.CreateRet(LLVM_GET_LONG(0));
					} else {
						ASSERT_NOT_REACHED();
					}
				} else {
					llvm_ctx.builder.CreateRetVoid();
				}
			} else {
				call->setTailCall(true);
				llvm_ctx.builder.CreateRet(call);
			}
		}
	}
	return 1;
}
/* }}} */

/* {{{ static int is_long_numeric_string */
static int is_long_numeric_string(const char *str, int length)
{
	const char *ptr;
	int base = 10, digits = 0, dp_or_e = 0;
	zend_uchar type;

	if (!length) {
		return 0;
	}

	/* Skip any whitespace
	 * This is much faster than the isspace() function */
	while (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r' || *str == '\v' || *str == '\f') {
		str++;
		length--;
	}
	ptr = str;

	if (*ptr == '-' || *ptr == '+') {
		ptr++;
	}

	if (ZEND_IS_DIGIT(*ptr)) {
		/* Handle hex numbers
		 * str is used instead of ptr to disallow signs and keep old behavior */
		if (length > 2 && *str == '0' && (str[1] == 'x' || str[1] == 'X')) {
			base = 16;
			ptr += 2;
		}

		/* Skip any leading 0s */
		while (*ptr == '0') {
			ptr++;
		}

		for (type = IS_LONG;; digits++, ptr++) {
check_digits:
			if (ZEND_IS_DIGIT(*ptr) || (base == 16 && ZEND_IS_XDIGIT(*ptr))) {
				continue;
			} else if (base == 10) {
				if (*ptr == '.' && dp_or_e < 1) {
					goto process_double;
				} else if ((*ptr == 'e' || *ptr == 'E') && dp_or_e < 2) {
					const char *e = ptr + 1;

					if (*e == '-' || *e == '+') {
						ptr = e++;
					}
					if (ZEND_IS_DIGIT(*e)) {
						goto process_double;
					}
				}
			}
			break;
		}

		if (base == 10) {
			if (digits >= MAX_LENGTH_OF_LONG) {
				dp_or_e = -1;
				goto process_double;
			}
		} else if (!(digits < SIZEOF_LONG * 2 || (digits == SIZEOF_LONG * 2 && ptr[-digits] <= '7'))) {
			type = IS_DOUBLE;
		}
	} else if (*ptr == '.' && ZEND_IS_DIGIT(ptr[1])) {
process_double:
		type = IS_DOUBLE;

		if (dp_or_e != -1) {
			dp_or_e = (*ptr++ == '.') ? 1 : 2;
			goto check_digits;
		}
	} else {
		return 0;
	}

	if (ptr != str + length) {
		return -1;
	}

	if (type == IS_DOUBLE) {
		return 0;
	}

	if (digits == MAX_LENGTH_OF_LONG - 1) {
		int cmp = strcmp(&ptr[-digits], long_min_digits);

		if (!(cmp < 0 || (cmp == 0 && *str == '-'))) {
			return 0;
		}
	}

	return 1;
}
/* }}} */

#define ZEND_JIT_HELPER_FAST_CALL      (1<<0)
#define ZEND_JIT_HELPER_VAR_ARGS       (1<<1)
#define ZEND_JIT_HELPER_READ_NONE      (1<<2)
#define ZEND_JIT_HELPER_READ_ONLY      (1<<3)

#define ZEND_JIT_HELPER_RET_NOALIAS    (1<<4)
#define ZEND_JIT_HELPER_ARG1_NOALIAS   (1<<5)
#define ZEND_JIT_HELPER_ARG1_NOCAPTURE (1<<6)
#define ZEND_JIT_HELPER_ARG2_NOALIAS   (1<<7)
#define ZEND_JIT_HELPER_ARG2_NOCAPTURE (1<<8)
#define ZEND_JIT_HELPER_ARG3_NOALIAS   (1<<9)
#define ZEND_JIT_HELPER_ARG3_NOCAPTURE (1<<10)
#define ZEND_JIT_HELPER_ARG4_NOALIAS   (1<<11)
#define ZEND_JIT_HELPER_ARG4_NOCAPTURE (1<<12)
#define ZEND_JIT_HELPER_ARG5_NOALIAS   (1<<13)
#define ZEND_JIT_HELPER_ARG5_NOCAPTURE (1<<14)


/* Proxy APIs */

#if JIT_EXCEPTION
/* {{{ static BasicBlock *zend_jit_find_exception_bb */
static BasicBlock *zend_jit_find_exception_bb(zend_llvm_ctx &ctx, zend_op *opline)
{
    int catch_bb_num = -1;
    uint32_t op_num = opline - ctx.op_array->opcodes;
    uint32_t catch_op_num = 0;

	for (int i = 0; i < ctx.op_array->last_try_catch; i++) {
		if (ctx.op_array->try_catch_array[i].try_op > op_num) {
			/* further blocks will not be relevant... */
			break;
		}
		if (op_num < ctx.op_array->try_catch_array[i].catch_op) {
			catch_op_num = ctx.op_array->try_catch_array[i].catch_op;
			catch_bb_num = i;
		}
		// FIXME: LLVM support for finally
		//if (op_num < ctx.op_array->try_catch_array[i].finally_op) {
		//	finally_op_num = ctx.op_array->try_catch_array[i].finally_op;
		//}
	}


	if (catch_bb_num >= 0) {
		if (!ctx.bb_exceptions[catch_bb_num]) {
			BasicBlock *bb = ctx.builder.GetInsertBlock();

			ctx.bb_exceptions[catch_bb_num] = BasicBlock::Create(ctx.context, "", ctx.function);
			ctx.builder.SetInsertPoint(ctx.bb_exceptions[catch_bb_num]);
			// Call HANDLE_EXCEPTION handler (non-tail call)
			JIT_CHECK(zend_jit_call_handler(ctx, EG(exception_op), 0));

			const zend_jit_func_info *info = JIT_DATA(ctx.op_array);
			for (int i = 0; i < info->cfg.blocks; i++) {
				if (info->cfg.block[i].start == catch_op_num) {
					ctx.builder.CreateBr(ctx.bb_labels[i]);
					break;
				}
			}
			
			ctx.builder.SetInsertPoint(bb);
		}
		return ctx.bb_exceptions[catch_bb_num];
	} else {
		if (!ctx.bb_exception_exit) {
			BasicBlock *bb = ctx.builder.GetInsertBlock();

			ctx.bb_exception_exit = BasicBlock::Create(ctx.context, "", ctx.function);
			ctx.builder.SetInsertPoint(ctx.bb_exception_exit);
			// Call HANDLE_EXCEPTION handler (tail call)
			JIT_CHECK(zend_jit_call_handler(ctx, EG(exception_op), 1));

			ctx.builder.SetInsertPoint(bb);
		}
		return ctx.bb_exception_exit;
	}
}
/* }}} */
#endif

/* {{{ static int zend_jit_store_opline */
static int zend_jit_store_opline(zend_llvm_ctx &llvm_ctx, zend_op *opline, bool update = 1)
{
	Constant *_opline = LLVM_GET_LONG((zend_uintptr_t)opline);
	llvm_ctx.builder.CreateAlignedStore(_opline, 
		zend_jit_GEP(
			llvm_ctx,
			llvm_ctx._execute_data,
			offsetof(zend_execute_data, opline),
			PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4);
	if (update) {
		llvm_ctx.valid_opline = 1;
	}
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_check_exception */
static int zend_jit_check_exception(zend_llvm_ctx &ctx, zend_op *opline)
{
#if JIT_EXCEPTION
    // FIXME: LLVM don't create empty basic blocks
	BasicBlock *bb_follow = BasicBlock::Create(ctx.context, "", ctx.function);
    BasicBlock *bb_exception = zend_jit_find_exception_bb(ctx, opline);

	zend_jit_unexpected_br(ctx,
		ctx.builder.CreateIsNotNull(
			ctx.builder.CreateAlignedLoad(ctx._EG_exception, 4, 1)),
		bb_exception,
		bb_follow);

	ctx.builder.SetInsertPoint(bb_follow);
#endif
	return 1;
}
/* }}} */

/* {{{ static inline Function* zend_jit_get_helper */
static inline Function* zend_jit_get_helper(zend_llvm_ctx &llvm_ctx,
                                            void          *addr,
                                            Twine          sym,
                                            uint32_t       flags,
                                            Type          *type_ret,
                                            Type          *type_arg1 = NULL,
                                            Type          *type_arg2 = NULL,
                                            Type          *type_arg3 = NULL,
                                            Type          *type_arg4 = NULL,
                                            Type          *type_arg5 = NULL)
{
	Function *_helper = const_cast<Function*>(cast_or_null<Function>(llvm_ctx.engine->getGlobalValueAtAddress(addr)));
	if (!_helper) {
		std::vector<llvm::Type *> args;
		if (type_arg1) args.push_back(type_arg1);
		if (type_arg2) args.push_back(type_arg2);
		if (type_arg3) args.push_back(type_arg3);
		if (type_arg4) args.push_back(type_arg4);
		if (type_arg5) args.push_back(type_arg5);
		_helper = Function::Create(
			FunctionType::get(
				type_ret,
				ArrayRef<Type*>(args),
				(flags & ZEND_JIT_HELPER_VAR_ARGS) ? 1 : 0),
			Function::ExternalLinkage,
			sym,
			llvm_ctx.module);
		if (flags & ZEND_JIT_HELPER_FAST_CALL) {
			_helper->setCallingConv(CallingConv::X86_FastCall);
#if (LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR == 2)
			if (type_arg1) {
				_helper->addAttribute(1,
					Attributes::get(llvm_ctx.context, Attributes::InReg));
			}
			if (type_arg2) {
				_helper->addAttribute(2,
					Attributes::get(llvm_ctx.context, Attributes::InReg));
			}
#else
			if (type_arg1) {
				_helper->addAttribute(1, Attribute::InReg);
			}
			if (type_arg2) {
				_helper->addAttribute(2, Attribute::InReg);
			}
#endif
		}
		if (flags & ZEND_JIT_HELPER_READ_NONE) {
			_helper->setDoesNotAccessMemory();
		}
		if (flags & ZEND_JIT_HELPER_READ_ONLY) {
			_helper->setOnlyReadsMemory();
		}
		if (flags & ZEND_JIT_HELPER_RET_NOALIAS) {
			_helper->setDoesNotAlias(0);
		}
		if (flags & ZEND_JIT_HELPER_ARG1_NOALIAS) {
			_helper->setDoesNotAlias(1);
		}
		if (flags & ZEND_JIT_HELPER_ARG1_NOCAPTURE) {
			_helper->setDoesNotCapture(1);
		}
		if (flags & ZEND_JIT_HELPER_ARG2_NOALIAS) {
			_helper->setDoesNotAlias(2);
		}
		if (flags & ZEND_JIT_HELPER_ARG2_NOCAPTURE) {
			_helper->setDoesNotCapture(2);
		}
		if (flags & ZEND_JIT_HELPER_ARG3_NOALIAS) {
			_helper->setDoesNotAlias(3);
		}
		if (flags & ZEND_JIT_HELPER_ARG3_NOCAPTURE) {
			_helper->setDoesNotCapture(3);
		}
		if (flags & ZEND_JIT_HELPER_ARG4_NOALIAS) {
			_helper->setDoesNotAlias(4);
		}
		if (flags & ZEND_JIT_HELPER_ARG4_NOCAPTURE) {
			_helper->setDoesNotCapture(4);
		}
		if (flags & ZEND_JIT_HELPER_ARG5_NOALIAS) {
			_helper->setDoesNotAlias(5);
		}
		if (flags & ZEND_JIT_HELPER_ARG5_NOCAPTURE) {
			_helper->setDoesNotCapture(5);
		}
		llvm_ctx.engine->addGlobalMapping(_helper, addr);
	}
	return _helper;
}
/* }}} */

/* {{{ static int zend_jit_handler */
static int zend_jit_handler(zend_llvm_ctx &ctx, zend_op *opline)
{
	if (!ctx.valid_opline) {
		// Store "opline" in EX(opline)
		JIT_CHECK(zend_jit_store_opline(ctx, opline));
	}

	// Call VM handler
	JIT_CHECK(zend_jit_call_handler(ctx, opline, 0));

	// Exception handling
	JIT_CHECK(zend_jit_check_exception(ctx, opline));
	return 1;
}
/* }}} */

/* {{{ static void zend_jit_error */
static void zend_jit_error(zend_llvm_ctx    &llvm_ctx,
                           zend_op          *opline,
                           int               type,
                           const char       *format,
                           Value            *arg1 = NULL,
                           Value            *arg2 = NULL,
                           Value            *arg3 = NULL)
{
	if (opline && !llvm_ctx.valid_opline) {
		zend_jit_store_opline(llvm_ctx, opline, false);
	}

	Function *_helper = zend_jit_get_helper(
		llvm_ctx,
		(void*)zend_error,
		ZEND_JIT_SYM("zend_error"),
		ZEND_JIT_HELPER_VAR_ARGS,
		Type::getVoidTy(llvm_ctx.context),
		Type::getInt32Ty(llvm_ctx.context),
		PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context)),
		NULL,
		NULL,
		NULL);
	if (arg3) {
		llvm_ctx.builder.CreateCall5(_helper,
			llvm_ctx.builder.getInt32(type),
			LLVM_GET_CONST_STRING(format),
			arg1, arg2, arg3);
	} else if (arg2) {
		llvm_ctx.builder.CreateCall4(_helper,
			llvm_ctx.builder.getInt32(type),
			LLVM_GET_CONST_STRING(format),
			arg1, arg2);
	} else if (arg1) {
		llvm_ctx.builder.CreateCall3(_helper,
			llvm_ctx.builder.getInt32(type),
			LLVM_GET_CONST_STRING(format),
			arg1);
	} else {
		llvm_ctx.builder.CreateCall2(_helper,
			llvm_ctx.builder.getInt32(type),
			LLVM_GET_CONST_STRING(format));
	}
}
/* }}} */

/* {{{ static void zend_jit_error_noreturn */
static void zend_jit_error_noreturn(zend_llvm_ctx    &llvm_ctx,
                                    zend_op          *opline,
                                    int               type,
                                    const char       *format,
                                    Value            *arg1 = NULL,
                                    Value            *arg2 = NULL,
                                    Value            *arg3 = NULL)
{
	CallInst *call;

	if (opline && !llvm_ctx.valid_opline) {
		zend_jit_store_opline(llvm_ctx, opline, false);
	}

	Function *_helper = zend_jit_get_helper(
		llvm_ctx,
		(void*)zend_error,
		ZEND_JIT_SYM("zend_error"),
		ZEND_JIT_HELPER_VAR_ARGS,
		Type::getVoidTy(llvm_ctx.context),
		Type::getInt32Ty(llvm_ctx.context),
		PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context)),
		NULL,
		NULL,
		NULL);
	if (arg3) {
		call = llvm_ctx.builder.CreateCall5(_helper,
			llvm_ctx.builder.getInt32(type),
			LLVM_GET_CONST_STRING(format),
			arg1, arg2, arg3);
	} else if (arg2) {
		call = llvm_ctx.builder.CreateCall4(_helper,
			llvm_ctx.builder.getInt32(type),
			LLVM_GET_CONST_STRING(format),
			arg1, arg2);
	} else if (arg1) {
		call = llvm_ctx.builder.CreateCall3(_helper,
			llvm_ctx.builder.getInt32(type),
			LLVM_GET_CONST_STRING(format),
			arg1);
	} else {
		call = llvm_ctx.builder.CreateCall2(_helper,
			llvm_ctx.builder.getInt32(type),
			LLVM_GET_CONST_STRING(format));
	}
	call->setDoesNotReturn();
	call->doesNotThrow();
	llvm_ctx.builder.CreateUnreachable();
}
/* }}} */

/* {{{ static void zend_jit_throw_error */
static void zend_jit_throw_error(zend_llvm_ctx    &llvm_ctx,
                                 zend_op          *opline,
                                 zend_class_entry *ce,
                                 const char       *format,
                                 Value            *arg1 = NULL,
                                 Value            *arg2 = NULL,
                                 Value            *arg3 = NULL)
{
	if (opline && !llvm_ctx.valid_opline) {
		zend_jit_store_opline(llvm_ctx, opline, false);
	}

	Function *_helper = zend_jit_get_helper(
		llvm_ctx,
		(void*)zend_throw_error,
		ZEND_JIT_SYM("zend_throw_error"),
		ZEND_JIT_HELPER_VAR_ARGS,
		Type::getVoidTy(llvm_ctx.context),
		PointerType::getUnqual(llvm_ctx.zend_class_entry_type),
		PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context)),
		NULL,
		NULL,
		NULL);
	if (arg3) {
		llvm_ctx.builder.CreateCall5(_helper,
			llvm_ctx.builder.CreateIntToPtr(
				LLVM_GET_LONG((zend_uintptr_t)ce),
				PointerType::getUnqual(llvm_ctx.zend_class_entry_type)),
			LLVM_GET_CONST_STRING(format),
			arg1, arg2, arg3);
	} else if (arg2) {
		llvm_ctx.builder.CreateCall4(_helper,
			llvm_ctx.builder.CreateIntToPtr(
				LLVM_GET_LONG((zend_uintptr_t)ce),
				PointerType::getUnqual(llvm_ctx.zend_class_entry_type)),
			LLVM_GET_CONST_STRING(format),
			arg1, arg2);
	} else if (arg1) {
		llvm_ctx.builder.CreateCall3(_helper,
			llvm_ctx.builder.CreateIntToPtr(
				LLVM_GET_LONG((zend_uintptr_t)ce),
				PointerType::getUnqual(llvm_ctx.zend_class_entry_type)),
			LLVM_GET_CONST_STRING(format),
			arg1);
	} else {
		llvm_ctx.builder.CreateCall2(_helper,
			llvm_ctx.builder.CreateIntToPtr(
				LLVM_GET_LONG((zend_uintptr_t)ce),
				PointerType::getUnqual(llvm_ctx.zend_class_entry_type)),
			LLVM_GET_CONST_STRING(format));
	}
}
/* }}} */

/* {{{ static Value *zend_jit_long_to_str */
static Value* zend_jit_long_to_str(zend_llvm_ctx &llvm_ctx,
                                   Value         *num)
{
	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)zend_long_to_str,
			ZEND_JIT_SYM("zend_long_to_str"),
			ZEND_JIT_HELPER_FAST_CALL,
			PointerType::getUnqual(llvm_ctx.zend_string_type),
			LLVM_GET_LONG_TY(llvm_ctx.context),
			NULL,
			NULL,
			NULL,
			NULL);

	CallInst *call = llvm_ctx.builder.CreateCall(_helper, num);
	call->setCallingConv(CallingConv::X86_FastCall);
	return call;
}
/* }}} */

/* {{{ static void zend_jit_locale_sprintf_double */
static void zend_jit_locale_sprintf_double(zend_llvm_ctx &llvm_ctx,
                                           Value         *zval_addr)
{
	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)zend_locale_sprintf_double,
			ZEND_JIT_SYM("zend_locale_sprintf_double"),
			ZEND_JIT_HELPER_FAST_CALL,
			PointerType::getVoidTy(llvm_ctx.context),
			llvm_ctx.zval_ptr_type,
			NULL,
			NULL,
			NULL,
			NULL);

	CallInst *call = llvm_ctx.builder.CreateCall(_helper, zval_addr);
	call->setCallingConv(CallingConv::X86_FastCall);
}
/* }}} */

/* {{{ static int zend_jit_zval_dtor_func */
static int zend_jit_zval_dtor_func(zend_llvm_ctx &llvm_ctx,
                                   Value         *counted,
                                   uint32_t       lineno)
{
	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)_zval_dtor_func,
			ZEND_JIT_SYM("_zval_dtor_func"),
			ZEND_JIT_HELPER_FAST_CALL | ZEND_JIT_HELPER_ARG1_NOALIAS | ZEND_JIT_HELPER_ARG1_NOCAPTURE,
			Type::getVoidTy(llvm_ctx.context),
			PointerType::getUnqual(llvm_ctx.zend_refcounted_type),
#if ZEND_DEBUG
			PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context)),
			Type::getInt32Ty(llvm_ctx.context),
#else
			NULL,
			NULL,
#endif
			NULL,
			NULL);

#if ZEND_DEBUG
	CallInst *call = llvm_ctx.builder.CreateCall3(
		_helper,
		counted,
		zend_jit_function_name(llvm_ctx),
		llvm_ctx.builder.getInt32(lineno));
#else
	CallInst *call = llvm_ctx.builder.CreateCall(
		_helper,
		counted);
#endif
	call->setCallingConv(CallingConv::X86_FastCall);

	return 1;
}
/* }}} */

/* {{{ static int zend_jit_gc_possible_root */
static int zend_jit_gc_possible_root(zend_llvm_ctx &llvm_ctx,
                                     Value         *counted)
{
	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)gc_possible_root,
			ZEND_JIT_SYM("gc_possible_root"),
			ZEND_JIT_HELPER_FAST_CALL | ZEND_JIT_HELPER_ARG1_NOALIAS | ZEND_JIT_HELPER_ARG1_NOCAPTURE,
			Type::getVoidTy(llvm_ctx.context),
			PointerType::getUnqual(llvm_ctx.zend_refcounted_type),
			NULL,
			NULL,
			NULL,
			NULL);

	CallInst *call = llvm_ctx.builder.CreateCall(
		_helper,
		counted);
	call->setCallingConv(CallingConv::X86_FastCall);

	return 1;
}
/* }}} */

/* {{{ static int zend_jit_copy_ctor_func */
static int zend_jit_copy_ctor_func(zend_llvm_ctx &llvm_ctx,
                                   Value         *zval_ptr,
                                   uint32_t       lineno)
{
	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)_zval_copy_ctor_func,
			ZEND_JIT_SYM("_zval_copy_ctor_func"),
			ZEND_JIT_HELPER_FAST_CALL | ZEND_JIT_HELPER_ARG1_NOALIAS | ZEND_JIT_HELPER_ARG1_NOCAPTURE,
			Type::getVoidTy(llvm_ctx.context),
			llvm_ctx.zval_ptr_type,
#if ZEND_DEBUG
			PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context)),
			Type::getInt32Ty(llvm_ctx.context),
#else
			NULL,
			NULL,
#endif
			NULL,
			NULL);

#if ZEND_DEBUG
	CallInst *call = llvm_ctx.builder.CreateCall3(
		_helper,
		zval_ptr,
		zend_jit_function_name(llvm_ctx),
		llvm_ctx.builder.getInt32(lineno));
#else
	CallInst *call = llvm_ctx.builder.CreateCall(
		_helper,
		zval_ptr);
#endif
	call->setCallingConv(CallingConv::X86_FastCall);

	return 1;
}
/* }}} */

/* {{{ static int zend_jit_zval_dtor_func_for_ptr */
static int zend_jit_zval_dtor_func_for_ptr(zend_llvm_ctx &llvm_ctx,
                                           Value         *counted,
                                           uint32_t       lineno)
{
	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)_zval_dtor_func_for_ptr,
			ZEND_JIT_SYM("_zval_dtor_func_for_ptr"),
			ZEND_JIT_HELPER_FAST_CALL | ZEND_JIT_HELPER_ARG1_NOALIAS | ZEND_JIT_HELPER_ARG1_NOCAPTURE,
			Type::getVoidTy(llvm_ctx.context),
			PointerType::getUnqual(llvm_ctx.zend_refcounted_type),
#if ZEND_DEBUG
			PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context)),
			Type::getInt32Ty(llvm_ctx.context),
#else
			NULL,
			NULL,
#endif
			NULL,
			NULL);

#if ZEND_DEBUG
	CallInst *call = llvm_ctx.builder.CreateCall3(
		_helper,
		counted,
		zend_jit_function_name(llvm_ctx),
		llvm_ctx.builder.getInt32(lineno));
#else
	CallInst *call = llvm_ctx.builder.CreateCall(
		_helper,
		counted);
#endif
	call->setCallingConv(CallingConv::X86_FastCall);

	return 1;
}
/* }}} */

/* {{{ static Value* zend_jit_is_true */
static Value* zend_jit_is_true(zend_llvm_ctx &llvm_ctx,
                               Value         *zval_addr)
{
	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)zend_is_true,
			ZEND_JIT_SYM("zend_is_true"),
			ZEND_JIT_HELPER_FAST_CALL | ZEND_JIT_HELPER_ARG1_NOALIAS | ZEND_JIT_HELPER_ARG1_NOCAPTURE,
			Type::getInt32Ty(llvm_ctx.context),
			llvm_ctx.zval_ptr_type,
			NULL,
			NULL,
			NULL,
			NULL);

	CallInst *call = llvm_ctx.builder.CreateCall(_helper, zval_addr);
	call->setCallingConv(CallingConv::X86_FastCall);
	return call;
}
/* }}} */

/* {{{ static void zend_jit_new_ref */
static void zend_jit_new_ref(zend_llvm_ctx &llvm_ctx,
                            Value         *ref_addr,
							Value         *val_addr) {
	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)zend_jit_helper_new_ref,
			ZEND_JIT_SYM("zend_jit_helper_new_ref"),
			ZEND_JIT_HELPER_FAST_CALL,
			Type::getVoidTy(llvm_ctx.context),
			llvm_ctx.zval_ptr_type,
			llvm_ctx.zval_ptr_type,
			NULL,
			NULL,
			NULL);

	CallInst *call = llvm_ctx.builder.CreateCall2(_helper, ref_addr, val_addr);
	call->setCallingConv(CallingConv::X86_FastCall);
}
/* }}} */

/* {{{ static void zend_jit_init_array */
static void zend_jit_init_array(zend_llvm_ctx &llvm_ctx,
                                Value         *zval_addr,
                                uint32_t       size)
{
	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)zend_jit_helper_init_array,
			ZEND_JIT_SYM("zend_jit_helper_init_array"),
			ZEND_JIT_HELPER_FAST_CALL,
			Type::getVoidTy(llvm_ctx.context),
			llvm_ctx.zval_ptr_type,
			Type::getInt32Ty(llvm_ctx.context),
			NULL,
			NULL,
			NULL);

	CallInst *call = llvm_ctx.builder.CreateCall2(_helper, zval_addr, llvm_ctx.builder.getInt32(size));
	call->setCallingConv(CallingConv::X86_FastCall);
}
/* }}} */

/* {{{ static void zend_jit_object_init */
static void zend_jit_object_init(zend_llvm_ctx  &llvm_ctx,
                                 Value          *zval_addr,
								 int             lineno)
{
	Function *_helper = zend_jit_get_helper(
		llvm_ctx,
		(void*)_object_init,
		ZEND_JIT_SYM("_object_init"),
		0,
		Type::getInt32Ty(llvm_ctx.context),
		llvm_ctx.zval_ptr_type,
#if ZEND_DEBUG
		// JIT: ZEND_FILE_LINE_CC
		PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context)),
		Type::getInt32Ty(llvm_ctx.context),
#else
		NULL,
		NULL,
#endif
		NULL,
		NULL);

#if ZEND_DEBUG
	llvm_ctx.builder.CreateCall3(_helper,
			zval_addr,
			zend_jit_function_name(llvm_ctx),
			llvm_ctx.builder.getInt32(lineno));
#else
	llvm_ctx.builder.CreateCall(_helper, zval_addr);
#endif
}
/* }}} */

/* {{{ static Value* zend_jit_strpprintf */
static Value* zend_jit_strpprintf(zend_llvm_ctx    &llvm_ctx,
                                  Value            *max_len,
                                  Value            *format,
                                  Value            *arg1,
                                  Value            *arg2 = NULL,
                                  Value            *arg3 = NULL)
{
	std::vector<llvm::Value *> params;
	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)strpprintf,
			ZEND_JIT_SYM("strpprintf"),
			ZEND_JIT_HELPER_VAR_ARGS,
			PointerType::getUnqual(llvm_ctx.zend_string_type),
			LLVM_GET_LONG_TY(llvm_ctx.context),
			PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context)),
			NULL,
			NULL,
			NULL);

	params.push_back(max_len);
	params.push_back(format);
	params.push_back(arg1);
	if (arg2) {
		params.push_back(arg2);
	}
	if (arg3) {
		params.push_back(arg3);
	}

	return llvm_ctx.builder.CreateCall(_helper, params);
}
/* }}} */

/* {{{ static Value* zend_jit_string_alloc */
static Value* zend_jit_string_alloc(zend_llvm_ctx    &llvm_ctx,
                                    Value            *len,
                                    int               persistent = 0)
{
	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)zend_jit_helper_string_alloc,
			ZEND_JIT_SYM("zend_jit_helper_string_alloc"),
//??? NOALIAS?
			ZEND_JIT_HELPER_FAST_CALL,
			PointerType::getUnqual(llvm_ctx.zend_string_type),
//??? Int32 -> ???
			LLVM_GET_LONG_TY(llvm_ctx.context),
			Type::getInt32Ty(llvm_ctx.context),
			NULL,
			NULL,
			NULL);

	CallInst *call = llvm_ctx.builder.CreateCall2(_helper,
		   	len,
			llvm_ctx.builder.getInt32(persistent));
	call->setCallingConv(CallingConv::X86_FastCall);
	return call;
}
/* }}} */

/* {{{ static Value* zend_jit_string_realloc */
static Value* zend_jit_string_realloc(zend_llvm_ctx    &llvm_ctx,
                                      Value            *str_addr,
                                      Value            *new_len,
                                      int               persistent = 0)
{
	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)zend_jit_helper_string_realloc,
			ZEND_JIT_SYM("zend_jit_helper_string_realloc"),
//??? NOALIAS?
			ZEND_JIT_HELPER_FAST_CALL,
			PointerType::getUnqual(llvm_ctx.zend_string_type),
			PointerType::getUnqual(llvm_ctx.zend_string_type),
			LLVM_GET_LONG_TY(llvm_ctx.context),
			Type::getInt32Ty(llvm_ctx.context),
			NULL,
			NULL);

	CallInst *call = llvm_ctx.builder.CreateCall3(_helper,
			str_addr,
		   	new_len,
			llvm_ctx.builder.getInt32(persistent));
	call->setCallingConv(CallingConv::X86_FastCall);
	return call;
#if 0
	int may_be_interned = 1;
	zend_op *op = opline;

	while (op > llvm_ctx.op_array->opcodes) {
		op--;
		if (op->result_type == IS_TMP_VAR && 
		    op->result.var == RES_OP()->var) {
			if (op->opcode == ZEND_ADD_VAR ||
			    op->opcode == ZEND_ADD_STRING) {
			    may_be_interned = (op->op1_type == IS_UNUSED);
			} else if (op->opcode == ZEND_ADD_CHAR) {
				may_be_interned = 0;
			} else if (op->opcode == ZEND_CONCAT) {
				may_be_interned = 0;
			} else {
				may_be_interned = 1;
			}
		    break;
		}
	}

	BasicBlock *bb_interned = NULL;
	BasicBlock *bb_not_interned = NULL;
	BasicBlock *bb_common = NULL;
	Value *str1 = NULL;

	if (may_be_interned) {
		bb_interned = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		bb_not_interned = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		bb_common = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

		// JIT: if (IS_INTERNED(Z_STRVAL_P(op1))) {
		zend_jit_is_interned(llvm_ctx, src_str, bb_interned, bb_not_interned);

		llvm_ctx.builder.SetInsertPoint(bb_interned);
		// JIT: buf = (char *) emalloc(length+1);
		str1 = llvm_ctx.builder.CreateBitCast(
				zend_jit_emalloc(llvm_ctx,
					llvm_ctx.builder.CreateZExt(
					llvm_ctx.builder.CreateAdd(
						len,
						llvm_ctx.builder.getInt32(1)),
					Type::LLVM_GET_LONG_TY(llvm_ctx.context)),
				opline->lineno),
			PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context)));
		// JIT: memcpy(buf, Z_STRVAL_P(op1), Z_STRLEN_P(op1));
		llvm_ctx.builder.CreateMemCpy(str1, src_str, src_len, 1);
		llvm_ctx.builder.CreateBr(bb_common);

		llvm_ctx.builder.SetInsertPoint(bb_not_interned);
	}

	// TODO JIT: buf = (char *) erealloc(Z_STRVAL_P(op1), length+1);
	Value *str = llvm_ctx.builder.CreateBitCast(
			zend_jit_erealloc(llvm_ctx,
				src_str,
				llvm_ctx.builder.CreateZExt(
					llvm_ctx.builder.CreateAdd(
						len,
						llvm_ctx.builder.getInt32(1)),
					Type::LLVM_GET_LONG_TY(llvm_ctx.context)),
				opline->lineno),
			PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context)));


	if (may_be_interned) {
		llvm_ctx.builder.CreateBr(bb_common);
		llvm_ctx.builder.SetInsertPoint(bb_common);
		PHINode *ret = llvm_ctx.builder.CreatePHI(PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context)), 2);
		ret->addIncoming(str1, bb_interned);
		ret->addIncoming(str, bb_not_interned);
		return ret;
	} else {
		call = llvm_ctx.builder.CreateCall2(_helper,
			llvm_ctx.builder.getInt32(type),
			LLVM_GET_CONST_STRING(format));
	}
	call->setDoesNotReturn();
	call->doesNotThrow();
	llvm_ctx.builder.CreateUnreachable();
#endif
}
/* }}} */

/* {{{ static Value* zend_jit_string_release */
static Value* zend_jit_string_release(zend_llvm_ctx    &llvm_ctx,
                                      Value            *str_addr)
{
	//TODO: inline this
	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)zend_jit_helper_string_release,
			ZEND_JIT_SYM("zend_jit_helper_string_release"),
			ZEND_JIT_HELPER_FAST_CALL,
			Type::getVoidTy(llvm_ctx.context),
			PointerType::getUnqual(llvm_ctx.zend_string_type),
			NULL,
			NULL,
			NULL,
			NULL);

	CallInst *call = llvm_ctx.builder.CreateCall(_helper, str_addr);
	call->setCallingConv(CallingConv::X86_FastCall);
	return call;
}
/* }}} */

/* {{{ static Value* zend_jit_zval_get_string_func */
static Value* zend_jit_zval_get_string_func(zend_llvm_ctx &llvm_ctx,
                                            Value         *zval_addr)
{
	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)_zval_get_string_func,
			ZEND_JIT_SYM("_zval_get_string_func"),
			ZEND_JIT_HELPER_FAST_CALL,
			PointerType::getUnqual(llvm_ctx.zend_string_type),
			llvm_ctx.zval_ptr_type,
			NULL,
			NULL,
			NULL,
			NULL);

	CallInst *call = llvm_ctx.builder.CreateCall(_helper, zval_addr);
	call->setCallingConv(CallingConv::X86_FastCall);
	return call;
}
/* }}} */

/* {{{ static Value* zend_jit_zval_get_lval_func */
static Value* zend_jit_zval_get_lval_func(zend_llvm_ctx &llvm_ctx,
                                          Value         *zval_addr)
{
	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)_zval_get_long_func,
			ZEND_JIT_SYM("_zval_get_long_func"),
			ZEND_JIT_HELPER_FAST_CALL,
			LLVM_GET_LONG_TY(llvm_ctx.context),
			llvm_ctx.zval_ptr_type,
			NULL,
			NULL,
			NULL,
			NULL);

	CallInst *call = llvm_ctx.builder.CreateCall(_helper, zval_addr);
	call->setCallingConv(CallingConv::X86_FastCall);
	return call;
}
/* }}} */

/* {{{ static Value* zend_jit_dval_to_lval */
static Value* zend_jit_dval_to_lval(zend_llvm_ctx &llvm_ctx,
									Value         *dval)
{
	Function *_helper = zend_jit_get_helper(
		llvm_ctx,
		(void*)zend_jit_helper_dval_to_lval,
		ZEND_JIT_SYM("zend_jit_helper_dval_to_lval"),
		0,
		Type::LLVM_GET_LONG_TY(llvm_ctx.context),
		Type::getDoubleTy(llvm_ctx.context),
		NULL,
		NULL,
		NULL,
		NULL);

	CallInst *call = llvm_ctx.builder.CreateCall(_helper, dval);

	return call;
}
/* }}} */

/* {{{ static int zend_jit_handle_numeric */
static void zend_jit_handle_numeric(zend_llvm_ctx &llvm_ctx,
                                    Value         *offset,
                                    Value         *index,
                                    BasicBlock    *bb_numeric,
                                    BasicBlock    *bb_string,
                                    zend_op       *opline)
{
	if (!llvm_ctx.valid_opline) {
		zend_jit_store_opline(llvm_ctx, opline, false);
	}

	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)zend_jit_helper_handle_numeric_str,
			ZEND_JIT_SYM("zend_jit_helper_handle_numeric_str"),
			ZEND_JIT_HELPER_FAST_CALL,
			Type::getInt32Ty(llvm_ctx.context),
			PointerType::getUnqual(llvm_ctx.zend_string_type),
			PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context)),
			NULL,
			NULL,
			NULL);

	CallInst *call = llvm_ctx.builder.CreateCall2(_helper, offset, index);
	call->setCallingConv(CallingConv::X86_FastCall);

	zend_jit_expected_br(
			llvm_ctx,
			llvm_ctx.builder.CreateICmpEQ(call, llvm_ctx.builder.getInt32(1)),
			bb_numeric,
			bb_string);
}
/* }}} */

/* {{{ static Value* zend_jit_slow_str_index */
static Value* zend_jit_slow_str_index(zend_llvm_ctx &llvm_ctx,
                                      Value         *dim,
                                      Value         *type,
									  zend_op       *opline)
{
	if (!llvm_ctx.valid_opline) {
		zend_jit_store_opline(llvm_ctx, opline, false);
	}

	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)zend_jit_helper_slow_str_index,
			ZEND_JIT_SYM("zend_jit_helper_slow_str_index"),
			ZEND_JIT_HELPER_FAST_CALL,
			LLVM_GET_LONG_TY(llvm_ctx.context),
			llvm_ctx.zval_ptr_type,
			Type::getInt32Ty(llvm_ctx.context),
			NULL,
			NULL,
			NULL);

	CallInst *call = llvm_ctx.builder.CreateCall2(_helper, dim, type);
	call->setCallingConv(CallingConv::X86_FastCall);
	return call;
}
/* }}} */

/* {{{ static Value* zend_jit_slow_fetch_address_obj */
static Value* zend_jit_slow_fetch_address_obj(zend_llvm_ctx  &llvm_ctx,
                                            Value          *obj,
                                            Value          *rv,
                                            Value          *result,
                                            zend_op        *opline)
{
	if (!llvm_ctx.valid_opline) {
		zend_jit_store_opline(llvm_ctx, opline, false);
	}

	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)zend_jit_helper_slow_fetch_address_obj,
			ZEND_JIT_SYM("zend_jit_helper_slow_fetch_address_obj"),
			ZEND_JIT_HELPER_FAST_CALL,
			Type::getInt32Ty(llvm_ctx.context),
			llvm_ctx.zval_ptr_type,
			llvm_ctx.zval_ptr_type,
			llvm_ctx.zval_ptr_type,
			NULL,
			NULL);

	CallInst *call = llvm_ctx.builder.CreateCall3(_helper, obj, rv, result);
	call->setCallingConv(CallingConv::X86_FastCall);

	return call;
}
/* }}} */

/* {{{ static Value* zend_jit_slow_strlen */
static Value* zend_jit_slow_strlen(zend_llvm_ctx  &llvm_ctx,
                                   Value          *val,
                                   Value          *ret)
{
	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)zend_jit_helper_slow_strlen,
			ZEND_JIT_SYM("zend_jit_helper_slow_strlen"),
			ZEND_JIT_HELPER_FAST_CALL,
			Type::getInt32Ty(llvm_ctx.context),
			PointerType::getUnqual(llvm_ctx.zend_execute_data_type),
			llvm_ctx.zval_ptr_type,
			llvm_ctx.zval_ptr_type,
			NULL,
			NULL);

	CallInst *call = llvm_ctx.builder.CreateCall3(_helper,
		llvm_ctx._execute_data, val, ret);
	call->setCallingConv(CallingConv::X86_FastCall);
	return call;
}
/* }}} */

/* {{{ static Value* zend_jit_read_dimension */
static Value* zend_jit_read_dimension(zend_llvm_ctx  &llvm_ctx,
                                      Value          *handler,
                                      Value          *obj,
                                      Value          *dim,
                                      uint32_t        fetch_type,
                                      Value          *rzv,
                                      zend_op        *opline)
{
	if (!llvm_ctx.valid_opline) {
		zend_jit_store_opline(llvm_ctx, opline, false);
	}

	std::vector<llvm::Type *> args;
	args.push_back(llvm_ctx.zval_ptr_type);
	args.push_back(llvm_ctx.zval_ptr_type);
	args.push_back(Type::getInt32Ty(llvm_ctx.context)); 
	args.push_back(llvm_ctx.zval_ptr_type);
	Type *func_t = FunctionType::get(
			llvm_ctx.zval_ptr_type,
			ArrayRef<Type*>(args),
			0);

	Value *helper = llvm_ctx.builder.CreateBitCast(
			handler,
			PointerType::getUnqual(func_t));

	CallInst *call = llvm_ctx.builder.CreateCall4(
			helper, 
			obj, 
			dim,
			llvm_ctx.builder.getInt32(fetch_type),
			rzv,
			ZEND_JIT_SYM("read_dimension"));

	return call;
}
/* }}} */

/* {{{ static void zend_jit_write_dimension */
static void zend_jit_write_dimension(zend_llvm_ctx  &llvm_ctx,
                                     Value          *handler,
                                     Value          *obj,
                                     Value          *dim,
                                     Value          *val,
                                     zend_op        *opline)
{
	if (!llvm_ctx.valid_opline) {
		zend_jit_store_opline(llvm_ctx, opline, false);
	}

	std::vector<llvm::Type *> args;
	args.push_back(llvm_ctx.zval_ptr_type);
	args.push_back(llvm_ctx.zval_ptr_type);
	args.push_back(llvm_ctx.zval_ptr_type);

	Type *func_t = FunctionType::get(
			Type::getVoidTy(llvm_ctx.context),
			ArrayRef<Type*>(args),
			0);

	Value *helper = llvm_ctx.builder.CreateBitCast(
			handler,
			PointerType::getUnqual(func_t));

	CallInst *call = llvm_ctx.builder.CreateCall3(
			helper, 
			obj, 
			dim,
			val);
}
/* }}} */

/* {{{ static void zend_jit_write_property */
static void zend_jit_write_property(zend_llvm_ctx  &llvm_ctx,
                                    Value          *handler,
                                    Value          *obj,
                                    Value          *dim,
                                    Value          *val,
									Value          *cache_slot,
                                    zend_op        *opline)
{
	if (!llvm_ctx.valid_opline) {
		zend_jit_store_opline(llvm_ctx, opline, false);
	}

	std::vector<llvm::Type *> args;
	args.push_back(llvm_ctx.zval_ptr_type);
	args.push_back(llvm_ctx.zval_ptr_type);
	args.push_back(llvm_ctx.zval_ptr_type);
	args.push_back(PointerType::getUnqual(PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))));

	Type *func_t = FunctionType::get(
			Type::getVoidTy(llvm_ctx.context),
			ArrayRef<Type*>(args),
			0);

	Value *helper = llvm_ctx.builder.CreateBitCast(
			handler,
			PointerType::getUnqual(func_t));

	CallInst *call = llvm_ctx.builder.CreateCall4(
			helper, 
			obj,
			dim,
			val,
			cache_slot);
}
/* }}} */

/* {{{ static Value* zend_jit_has_dimension */
static Value* zend_jit_has_dimension(zend_llvm_ctx  &llvm_ctx,
                                     Value          *handler,
                                     Value          *obj,
                                     Value          *dim,
                                     int             check_empty,
                                     zend_op        *opline)
{
	if (!llvm_ctx.valid_opline) {
		zend_jit_store_opline(llvm_ctx, opline, false);
	}

	std::vector<llvm::Type *> args;
	args.push_back(llvm_ctx.zval_ptr_type);
	args.push_back(llvm_ctx.zval_ptr_type);
	args.push_back(Type::getInt32Ty(llvm_ctx.context));

	Type *func_t = FunctionType::get(
			Type::getInt32Ty(llvm_ctx.context),
			ArrayRef<Type*>(args),
			0);

	Value *helper = llvm_ctx.builder.CreateBitCast(
			handler,
			PointerType::getUnqual(func_t));

	CallInst *call = llvm_ctx.builder.CreateCall3(
			helper, 
			obj,
			dim,
			llvm_ctx.builder.getInt32(check_empty));
	return call;
}
/* }}} */

/* {{{ static Value* zend_jit_do_operation */
static Value* zend_jit_do_operation(zend_llvm_ctx  &llvm_ctx,
                                    Value          *handler,
                                    zend_uchar      opcode,
                                    Value          *result,
                                    Value          *op1,
                                    Value          *op2,
                                    zend_op        *opline)
{
	if (!llvm_ctx.valid_opline) {
		zend_jit_store_opline(llvm_ctx, opline, false);
	}

	std::vector<llvm::Type *> args;
	args.push_back(Type::getInt8Ty(llvm_ctx.context)); 
	args.push_back(llvm_ctx.zval_ptr_type);
	args.push_back(llvm_ctx.zval_ptr_type);
	args.push_back(llvm_ctx.zval_ptr_type);
	Type *func_t = FunctionType::get(
			Type::getInt32Ty(llvm_ctx.context),
			ArrayRef<Type*>(args),
			0);

	Value *helper = llvm_ctx.builder.CreateBitCast(
			handler,
			PointerType::getUnqual(func_t));

	CallInst *call = llvm_ctx.builder.CreateCall4(
			helper, 
			llvm_ctx.builder.getInt8(opcode), 
			result,
			op1,
			op2,
			ZEND_JIT_SYM("do_operation"));

	return call;
}
/* }}} */

/* {{{ static void zend_jit_assign_to_string_offset */
static void zend_jit_assign_to_string_offset(zend_llvm_ctx  &llvm_ctx,
                                             Value          *str,
                                             Value          *offset,
                                             Value          *val,
                                             Value          *result,
                                             zend_op        *opline)
{
	if (!llvm_ctx.valid_opline) {
		zend_jit_store_opline(llvm_ctx, opline, false);
	}

	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)zend_jit_helper_assign_to_string_offset,
			ZEND_JIT_SYM("zend_jit_helper_assign_to_string_offset"),
			ZEND_JIT_HELPER_FAST_CALL,
			Type::getVoidTy(llvm_ctx.context),
			llvm_ctx.zval_ptr_type,
			LLVM_GET_LONG_TY(llvm_ctx.context),
			llvm_ctx.zval_ptr_type,
			llvm_ctx.zval_ptr_type,
			NULL);

	CallInst *call = llvm_ctx.builder.CreateCall4(_helper, str, offset, val, result);
	call->setCallingConv(CallingConv::X86_FastCall);
}
/* }}} */

/* {{{ static Value* zend_jit_obj_proxy_op */
static Value* zend_jit_obj_proxy_op(zend_llvm_ctx &llvm_ctx,
                                    Value         *obj,
                                    Value         *val,
                                    zend_uchar     opcode,
                                    zend_op       *opline)
{
	void *helper;
	const char *name;

	switch (opcode) {
		case ZEND_ADD:
			helper = (void*)zend_jit_obj_proxy_add;
			name = ZEND_JIT_SYM("zend_jit_obj_proxy_add");
			break;
		case ZEND_SUB:
			helper = (void*)zend_jit_obj_proxy_sub;
			name = ZEND_JIT_SYM("zend_jit_obj_proxy_sub");
			break;
		case ZEND_MUL:
			helper = (void*)zend_jit_obj_proxy_mul;
			name = ZEND_JIT_SYM("zend_jit_obj_proxy_mul");
			break;
		case ZEND_DIV:
			helper = (void*)zend_jit_obj_proxy_div;
			name = ZEND_JIT_SYM("zend_jit_obj_proxy_div");
			break;
		case ZEND_CONCAT:
			helper = (void*)zend_jit_obj_proxy_concat;
			name = ZEND_JIT_SYM("zend_jit_obj_proxy_concat");
			break;
		default:
			ASSERT_NOT_REACHED();
	}

	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			helper,
			name,
			ZEND_JIT_HELPER_FAST_CALL | ZEND_JIT_HELPER_ARG1_NOALIAS | ZEND_JIT_HELPER_ARG1_NOCAPTURE,
			llvm_ctx.zval_ptr_type,
			llvm_ctx.zval_ptr_type,
			llvm_ctx.zval_ptr_type);
	
	CallInst *call = llvm_ctx.builder.CreateCall2(_helper, obj, val);

	call->setCallingConv(CallingConv::X86_FastCall);

	return call;
}
/* }}} */

/* {{{ static int zend_jit_memcmp */
static Value* zend_jit_memcmp(zend_llvm_ctx    &llvm_ctx,
                              Value            *s1,
                              Value            *s2,
                              Value            *len)
{
	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)memcmp,
			ZEND_JIT_SYM("memcmp"),
			0,
			Type::getInt32Ty(llvm_ctx.context),
			PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context)),
			PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context)),
			LLVM_GET_LONG_TY(llvm_ctx.context),
			NULL,
			NULL);

	return llvm_ctx.builder.CreateCall3(_helper, s1, s2, len);
}
/* }}} */

/* Common APIs */

/* {{{ static int zend_jit_throw_exception */
static int zend_jit_throw_exception(zend_llvm_ctx &ctx, zend_op *opline)
{
#if JIT_EXCEPTION
    BasicBlock *bb_exception = zend_jit_find_exception_bb(ctx, opline);
	ctx.builder.CreateBr(bb_exception);
#endif
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_tail_handler */
static int zend_jit_tail_handler(zend_llvm_ctx &ctx, zend_op *opline)
{
	if (!ctx.valid_opline) {
		// Store "opline" in EX(opline)
		JIT_CHECK(zend_jit_store_opline(ctx, opline));
	}

	// Call VM handler (tail call)
	JIT_CHECK(zend_jit_call_handler(ctx, opline, 1));

	return 1;
}
/* }}} */

/* {{{ static int zend_jit_cond_jmp */
static int zend_jit_cond_jmp(zend_llvm_ctx &llvm_ctx, zend_op *opline, zend_op *target, BasicBlock *l1, BasicBlock *l2)
{
	zend_jit_unexpected_br(llvm_ctx,
		llvm_ctx.builder.CreateICmpEQ(
			llvm_ctx.builder.CreateAlignedLoad(
				zend_jit_GEP(
					llvm_ctx,
					llvm_ctx._execute_data,
					offsetof(zend_execute_data, opline),
					PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4),
			LLVM_GET_LONG((zend_uintptr_t)(target))),
		l1,
		l2);
	return 1;
}
/* }}} */

/* {{{ static Value *zend_jit_get_stack_slot */
static Value *zend_jit_get_stack_slot(zend_llvm_ctx &llvm_ctx, int n)
{
	if (!llvm_ctx.stack_slots[n]) {
		IRBuilder<> Tmp(
				&llvm_ctx.function->getEntryBlock(),
				llvm_ctx.function->getEntryBlock().begin());
		AllocaInst *inst =
			Tmp.CreateAlloca(
					llvm_ctx.zval_type,
					LLVM_GET_LONG(sizeof(zval)/sizeof(long)),
					"stack_slot");
		inst->setAlignment(4);
		llvm_ctx.stack_slots[n] = inst;
	}

	return llvm_ctx.stack_slots[n];
}
/* }}} */

/* {{{ static Value* zend_jit_load_const */
static Value* zend_jit_load_const(zend_llvm_ctx &llvm_ctx, zval *zv)
{
	return llvm_ctx.builder.CreateIntToPtr(
		LLVM_GET_LONG((zend_uintptr_t)zv),
		llvm_ctx.zval_ptr_type);
}
/* }}} */

/* {{{ static Value* zend_jit_load_slot */
static Value* zend_jit_load_slot(zend_llvm_ctx &llvm_ctx, int var)
{
	return zend_jit_GEP(
			llvm_ctx,
			llvm_ctx._execute_data,
			var,
			llvm_ctx.zval_ptr_type);
}
/* }}} */

/* {{{ static Value* zend_jit_load_tmp_zval */
static Value* zend_jit_load_tmp_zval(zend_llvm_ctx &llvm_ctx, uint32_t var)
{
	return zend_jit_load_slot(llvm_ctx, var);
}
/* }}} */

/* {{{ static Value* zend_jit_load_var */
static Value* zend_jit_load_var(zend_llvm_ctx &llvm_ctx, uint32_t var)
{
	return zend_jit_load_slot(llvm_ctx, var);
}
/* }}} */

/* {{{ static Value* zend_jit_load_cv_addr */
static Value* zend_jit_load_cv_addr(zend_llvm_ctx &llvm_ctx, uint32_t var)
{
	return zend_jit_load_slot(llvm_ctx, var);
}
/* }}} */

/* {{{ static Value* zend_jit_load_type_info */
static Value* zend_jit_load_type_info(zend_llvm_ctx &llvm_ctx,
                                      Value         *zval_addr,
                                      int            ssa_var,
                                      uint32_t       info)
{
	if (info & (MAY_BE_NULL|MAY_BE_FALSE|MAY_BE_TRUE)) {
		if ((info & MAY_BE_ANY) == MAY_BE_NULL) {
			return llvm_ctx.builder.getInt32(IS_NULL);
		} else if ((info & MAY_BE_ANY) == MAY_BE_FALSE) {
			return llvm_ctx.builder.getInt32(IS_FALSE);
		} else if ((info & MAY_BE_ANY) == MAY_BE_TRUE) {
			return llvm_ctx.builder.getInt32(IS_TRUE);
		} else if (info & MAY_BE_IN_REG) {
			return llvm_ctx.builder.CreateAlignedLoad(llvm_ctx.reg[ssa_var], 4);
		} 
	} else if ((info & MAY_BE_ANY) == MAY_BE_LONG) {
		return llvm_ctx.builder.getInt32(IS_LONG);
	} else if ((info & MAY_BE_ANY) == MAY_BE_DOUBLE) {
		return llvm_ctx.builder.getInt32(IS_DOUBLE);
//???	} else if ((info & MAY_BE_ANY) == MAY_BE_ARRAY) {
//???		return llvm_ctx.builder.getInt8(IS_ARRAY);
	} else if ((info & MAY_BE_ANY) == MAY_BE_OBJECT) {
		return llvm_ctx.builder.getInt32(IS_OBJECT_EX);
//???		} else if ((info & MAY_BE_ANY) == MAY_BE_STRING) {
//???			return llvm_ctx.builder.getInt8(IS_STRING);
	} else if ((info & MAY_BE_ANY) == MAY_BE_RESOURCE) {
		return llvm_ctx.builder.getInt32(IS_RESOURCE_EX);
	} 
	return llvm_ctx.builder.CreateAlignedLoad(
			zend_jit_GEP(
				llvm_ctx,
				zval_addr,
				offsetof(zval,u1.type_info),
				PointerType::getUnqual(Type::getInt32Ty(llvm_ctx.context))), 4);
}
/* }}} */

/* {{{ static int zend_jit_save_zval_type_info */
static int zend_jit_save_zval_type_info(zend_llvm_ctx &llvm_ctx,
                                        Value         *zval_addr,
                                        int            ssa_var,
                                        uint32_t       info,
                                        Value         *type)
{
	if (info & MAY_BE_IN_REG) {
		if (info & (MAY_BE_NULL|MAY_BE_FALSE|MAY_BE_TRUE)) {
			llvm_ctx.builder.CreateAlignedStore(type, llvm_ctx.reg[ssa_var], 4);
		}
		return 1;
	}
	llvm_ctx.builder.CreateAlignedStore(
		type,
		zend_jit_GEP(
			llvm_ctx,
			zval_addr,
			offsetof(zval, u1.type_info),
			PointerType::getUnqual(Type::getInt32Ty(llvm_ctx.context))),
		4);
	return 1;
}
/* }}} */

/* {{{ static Value* zend_jit_load_type_flags */
static Value* zend_jit_load_type_flags(zend_llvm_ctx &llvm_ctx,
                                      Value         *zval_addr,
                                      int            ssa_var,
                                      uint32_t       info)
{
	if ((info & MAY_BE_ANY) == MAY_BE_NULL) {
		return llvm_ctx.builder.getInt8(0);
	} else if ((info & MAY_BE_ANY) == MAY_BE_FALSE) {
		return llvm_ctx.builder.getInt8(0);
	} else if ((info & MAY_BE_ANY) == MAY_BE_TRUE) {
		return llvm_ctx.builder.getInt8(0);
	} else if ((info & MAY_BE_ANY) == MAY_BE_LONG) {
		return llvm_ctx.builder.getInt8(0);
	} else if ((info & MAY_BE_ANY) == MAY_BE_DOUBLE) {
		return llvm_ctx.builder.getInt8(0);
//	} else if ((info & MAY_BE_ANY) == MAY_BE_ARRAY) {
//		return llvm_ctx.builder.getInt8(IS_ARRAY);
	} else if ((info & MAY_BE_ANY) == MAY_BE_OBJECT) {
		return llvm_ctx.builder.getInt8((IS_OBJECT_EX >> 8) & 0xff);
//	} else if ((info & MAY_BE_ANY) == MAY_BE_STRING) {
//		return llvm_ctx.builder.getInt8(IS_STRING);
	} else if ((info & MAY_BE_ANY) == MAY_BE_RESOURCE) {
		return llvm_ctx.builder.getInt8((IS_RESOURCE_EX >> 8) & 0xff);
	}
	return llvm_ctx.builder.CreateTruncOrBitCast(
		llvm_ctx.builder.CreateLShr(
			zend_jit_load_type_info(llvm_ctx, zval_addr, ssa_var, info),
			llvm_ctx.builder.getInt32(8)),
		Type::getInt8Ty(llvm_ctx.context));
}
/* }}} */

/* {{{ static Value* zend_jit_load_var_flags */
static Value* zend_jit_load_var_flags(zend_llvm_ctx &llvm_ctx,
                                      Value         *zval_addr)
{
	return llvm_ctx.builder.CreateAlignedLoad(
			zend_jit_GEP(
				llvm_ctx,
				zval_addr,
				offsetof(zval,u2.var_flags),
				PointerType::getUnqual(Type::getInt32Ty(llvm_ctx.context))), 4);
}
/* }}} */

/* {{{ static Value* zend_jit_load_type */
static Value* zend_jit_load_type(zend_llvm_ctx &llvm_ctx,
                                 Value         *zval_addr,
                                 int            ssa_var,
                                 uint32_t       info)
{
	if ((info & MAY_BE_ANY) == MAY_BE_NULL) {
		return llvm_ctx.builder.getInt8(IS_NULL);
	} else if ((info & MAY_BE_ANY) == MAY_BE_FALSE) {
		return llvm_ctx.builder.getInt8(IS_FALSE);
	} else if ((info & MAY_BE_ANY) == MAY_BE_TRUE) {
		return llvm_ctx.builder.getInt8(IS_TRUE);
	} else if ((info & MAY_BE_ANY) == MAY_BE_LONG) {
		return llvm_ctx.builder.getInt8(IS_LONG);
	} else if ((info & MAY_BE_ANY) == MAY_BE_DOUBLE) {
		return llvm_ctx.builder.getInt8(IS_DOUBLE);
	} else if ((info & MAY_BE_ANY) == MAY_BE_ARRAY) {
		return llvm_ctx.builder.getInt8(IS_ARRAY);
	} else if ((info & MAY_BE_ANY) == MAY_BE_OBJECT) {
		return llvm_ctx.builder.getInt8(IS_OBJECT);
	} else if ((info & MAY_BE_ANY) == MAY_BE_STRING) {
		return llvm_ctx.builder.getInt8(IS_STRING);
	} else if ((info & MAY_BE_ANY) == MAY_BE_RESOURCE) {
		return llvm_ctx.builder.getInt8(IS_RESOURCE);
	}
	return llvm_ctx.builder.CreateTruncOrBitCast(
		zend_jit_load_type_info(llvm_ctx, zval_addr, ssa_var, info),
		Type::getInt8Ty(llvm_ctx.context));
}
/* }}} */

/* {{{ static Value* zend_jit_load_type_c */
static Value* zend_jit_load_type_c(zend_llvm_ctx &llvm_ctx,
                                   Value         *zval_addr,
                                   zend_uchar     op_type,
                                   znode_op      *op,
                                   int            ssa_var,
                                   uint32_t       info)
{
    if (op_type == IS_CONST) {
		return llvm_ctx.builder.getInt8(Z_TYPE_P(RT_CONSTANT(llvm_ctx.op_array, *op)));
	} else {
		return zend_jit_load_type(llvm_ctx, zval_addr, ssa_var, info);
	}
}
/* }}} */

/* {{{ static Value* zend_jit_load_type_info_c */
static Value* zend_jit_load_type_info_c(zend_llvm_ctx &llvm_ctx,
                                        Value         *zval_addr,
                                        zend_uchar     op_type,
                                        znode_op      *op,
                                        int            ssa_var,
                                        uint32_t       info)
{
    if (op_type == IS_CONST) {
		return llvm_ctx.builder.getInt32(Z_TYPE_INFO_P(RT_CONSTANT(llvm_ctx.op_array, *op)));
	}
	return zend_jit_load_type_info(llvm_ctx, zval_addr, ssa_var, info);
}
/* }}} */

/* {{{ static int zend_jit_save_zval_lval */
static int zend_jit_save_zval_lval(zend_llvm_ctx &llvm_ctx,
                                   Value         *zval_addr,
                                   int            ssa_var,
                                   uint32_t       info,
                                   Value         *val)
{
	if (info & MAY_BE_IN_REG) {
		llvm_ctx.builder.CreateAlignedStore(val, llvm_ctx.reg[ssa_var], 4);
		return 1;
	}
	llvm_ctx.builder.CreateAlignedStore(
		val,
		zend_jit_GEP(
			llvm_ctx,
			zval_addr,
			offsetof(zval,value.lval),
			PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))),
		4);
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_save_zval_zvalue */
static int zend_jit_save_zval_value(zend_llvm_ctx &llvm_ctx,
                                    Value       *zval_addr,
                                    Value       *val1,
                                    Value       *val2)
{
	// - 1st store is sizeof(long) width at offset 0
	// - 2nd store is sizeof(long) width at offset (0 + sizeof(long)).
	llvm_ctx.builder.CreateAlignedStore(
		val1,
		zend_jit_GEP(
			llvm_ctx,
			zval_addr,
			offsetof(zval,value),
			PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))),
		4);
	llvm_ctx.builder.CreateAlignedStore(
		val2,
		zend_jit_GEP(
			llvm_ctx,
			zval_addr,
			offsetof(zval,value) + sizeof(long),
			PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))),
		4);
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_save_zval_str */
static int zend_jit_save_zval_str(zend_llvm_ctx &llvm_ctx,
                                  Value         *zval_addr,
                                  int            ssa_var,
                                  uint32_t       info,
                                  Value         *str_addr)
{
	if (info & MAY_BE_IN_REG) {
		llvm_ctx.builder.CreateAlignedStore(str_addr, llvm_ctx.reg[ssa_var], 4);
		return 1;
	}
	llvm_ctx.builder.CreateAlignedStore(
		str_addr,
		zend_jit_GEP(
			llvm_ctx,
			zval_addr,
			offsetof(zval, value.str),
			PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.zend_string_type))), 4);
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_save_zval_dval */
static int zend_jit_save_zval_dval(zend_llvm_ctx &llvm_ctx,
                                   Value         *zval_addr,
                                   int            ssa_var,
                                   uint32_t       info,
                                   Value         *val)
{
	if (info & MAY_BE_IN_REG) {
		llvm_ctx.builder.CreateAlignedStore(val, llvm_ctx.reg[ssa_var], 4);
		return 1;
	}
	llvm_ctx.builder.CreateAlignedStore(
		val,
		zend_jit_GEP(
			llvm_ctx,
			zval_addr,
			offsetof(zval,value.dval),
			PointerType::getUnqual(Type::getDoubleTy(llvm_ctx.context))),
		4);
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_save_zval_obj */
static int zend_jit_save_zval_obj(zend_llvm_ctx &llvm_ctx,
                                  Value         *zval_addr,
                                  int            ssa_var,
                                  uint32_t       info,
                                  Value         *val)
{
	if (info & MAY_BE_IN_REG) {
		llvm_ctx.builder.CreateAlignedStore(val, llvm_ctx.reg[ssa_var], 4);
		return 1;
	}
	llvm_ctx.builder.CreateAlignedStore(
		val,
		zend_jit_GEP(
			llvm_ctx,
			zval_addr,
			offsetof(zval, value.obj),
			PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.zend_object_type))),
		4);
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_save_zval_ptr */
static int zend_jit_save_zval_ptr(zend_llvm_ctx &llvm_ctx,
                                  Value         *zval_addr,
                                  int            ssa_var,
                                  uint32_t       info,
                                  Value         *val)
{
	if (info & MAY_BE_IN_REG) {
		llvm_ctx.builder.CreateAlignedStore(
			llvm_ctx.builder.CreateBitCast(
				val,
				PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context))),
			llvm_ctx.builder.CreateBitCast(
				llvm_ctx.reg[ssa_var],
				PointerType::getUnqual(PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context)))),
			4);
		return 1;
	}
	llvm_ctx.builder.CreateAlignedStore(
		llvm_ctx.builder.CreateBitCast(
			val,
			PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context))),
		zend_jit_GEP(
			llvm_ctx,
			zval_addr,
			offsetof(zval, value.obj),
			PointerType::getUnqual(PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context)))),
		4);
	return 1;
}
/* }}} */

/* {{{ static Value* zend_jit_load_ptr */
static Value* zend_jit_load_ptr(zend_llvm_ctx &llvm_ctx,
                                Value         *zval_addr,
                                int            ssa_var,
                                uint32_t       info)
{
	if (info & MAY_BE_IN_REG) {
		return 	llvm_ctx.builder.CreateBitCast(
			llvm_ctx.builder.CreateAlignedLoad(llvm_ctx.reg[ssa_var], 4),
			PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context)));
	}
	return llvm_ctx.builder.CreateAlignedLoad(
			zend_jit_GEP(
				llvm_ctx,
				zval_addr,
				offsetof(zval, value.obj),
				PointerType::getUnqual(
					PointerType::getUnqual(
						Type::getInt8Ty(llvm_ctx.context)))), 4);
}
/* }}} */

/* {{{ static int zend_jit_save_zval_ind */
static int zend_jit_save_zval_ind(zend_llvm_ctx &llvm_ctx,
                                  Value         *zval_addr,
                                  Value         *val)
{
	llvm_ctx.builder.CreateAlignedStore(
		val,
		zend_jit_GEP(
			llvm_ctx,
			zval_addr,
			offsetof(zval, value.zv),
			PointerType::getUnqual((llvm_ctx.zval_ptr_type))),
		4);
	return 1;
}
/* }}} */

/* {{{ static Value* zend_jit_load_counted */
static Value* zend_jit_load_counted(zend_llvm_ctx &llvm_ctx,
                                    Value         *zval_addr,
                                    int            ssa_var,
                                    uint32_t       info)
{
	if (info & MAY_BE_IN_REG) {
		return 	llvm_ctx.builder.CreateBitCast(
			llvm_ctx.builder.CreateAlignedLoad(llvm_ctx.reg[ssa_var], 4),
			PointerType::getUnqual(llvm_ctx.zend_refcounted_type));
	}
	return llvm_ctx.builder.CreateAlignedLoad(
			zend_jit_GEP(
				llvm_ctx,
				zval_addr,
				offsetof(zval,value.counted),
				PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.zend_refcounted_type))),
			4);
}
/* }}} */

/* {{{ static Value* zend_jit_load_reference */
static Value* zend_jit_load_reference(zend_llvm_ctx &llvm_ctx, Value *counted)
{
	return zend_jit_GEP(
			llvm_ctx,
			counted,
			offsetof(zend_reference, val),
			llvm_ctx.zval_ptr_type);
}
/* }}} */

/* {{{ static Value* zend_jit_refcount_addr */
static Value* zend_jit_refcount_addr(zend_llvm_ctx &llvm_ctx, Value *counted)
{
	return zend_jit_GEP(
			llvm_ctx,
			counted,
			offsetof(zend_refcounted, gc.refcount),
			PointerType::getUnqual(Type::getInt32Ty(llvm_ctx.context)));
}
/* }}} */

/* {{{ static Value* zend_jit_delref */
static Value* zend_jit_delref(zend_llvm_ctx &llvm_ctx, Value *counted)
{
	Value *refcount_addr = zend_jit_refcount_addr(llvm_ctx, counted);
	Value *new_val = llvm_ctx.builder.CreateSub(
			llvm_ctx.builder.CreateAlignedLoad(refcount_addr, 4),
			llvm_ctx.builder.getInt32(1));
	llvm_ctx.builder.CreateAlignedStore(
		new_val,
		refcount_addr,
		4);
	return new_val;
}
/* }}} */

/* {{{ static Value* zend_jit_addref */
static Value* zend_jit_addref(zend_llvm_ctx &llvm_ctx, Value *counted)
{
	Value *refcount_addr = zend_jit_refcount_addr(llvm_ctx, counted);
	Value *new_val = llvm_ctx.builder.CreateAdd(
			llvm_ctx.builder.CreateAlignedLoad(refcount_addr, 4),
			llvm_ctx.builder.getInt32(1));
	llvm_ctx.builder.CreateAlignedStore(
		new_val,
		refcount_addr,
		4);
	return new_val;
}
/* }}} */

/* {{{ static Value* zend_jit_load_lval */
static Value* zend_jit_load_lval(zend_llvm_ctx &llvm_ctx,
                                 Value         *zval_addr,
                                 int            ssa_var,
                                 uint32_t       info)
{
	if (info & MAY_BE_IN_REG) {
		return llvm_ctx.builder.CreateAlignedLoad(llvm_ctx.reg[ssa_var], 4);
	}
	return llvm_ctx.builder.CreateAlignedLoad(
			zend_jit_GEP(
				llvm_ctx,
				zval_addr,
				offsetof(zval,value.lval),
				PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))),
			4);
}
/* }}} */

/* {{{ static Value* zend_jit_load_lval_c */
static Value* zend_jit_load_lval_c(zend_llvm_ctx &llvm_ctx,
                                   Value         *zval_addr,
                                   zend_uchar     op_type,
                                   znode_op      *op,
                                   int            ssa_var,
                                   uint32_t       info)
{
	if (op_type == IS_CONST) {
		return LLVM_GET_LONG(Z_LVAL_P(RT_CONSTANT(llvm_ctx.op_array, *op)));
	} else if (ssa_var >=0 &&
	    ((JIT_DATA(llvm_ctx.op_array)->ssa_var_info[ssa_var].type & MAY_BE_ANY) == MAY_BE_LONG) &&
		JIT_DATA(llvm_ctx.op_array)->ssa_var_info[ssa_var].has_range &&
		JIT_DATA(llvm_ctx.op_array)->ssa_var_info[ssa_var].range.min ==
			JIT_DATA(llvm_ctx.op_array)->ssa_var_info[ssa_var].range.max) {
		return LLVM_GET_LONG(JIT_DATA(llvm_ctx.op_array)->ssa_var_info[ssa_var].range.min);
	} else {
		return zend_jit_load_lval(llvm_ctx, zval_addr, ssa_var, info);
	}
}
/* }}} */

/* {{{ static Value* zend_jit_load_dval */
static Value* zend_jit_load_dval(zend_llvm_ctx &llvm_ctx,
                                 Value         *zval_addr,
                                 int            ssa_var,
                                 uint32_t       info)
{
	if (info & MAY_BE_IN_REG) {
		return llvm_ctx.builder.CreateAlignedLoad(llvm_ctx.reg[ssa_var], 4);
	}
	return llvm_ctx.builder.CreateAlignedLoad(
			zend_jit_GEP(
				llvm_ctx,
				zval_addr,
				offsetof(zval,value.dval),
				PointerType::getUnqual(Type::getDoubleTy(llvm_ctx.context))),
			4);
}
/* }}} */

/* {{{ static Value* zend_jit_load_ind */
static Value* zend_jit_load_ind(zend_llvm_ctx &llvm_ctx,
                                Value         *zval_addr)
{
	return llvm_ctx.builder.CreateAlignedLoad(
			zend_jit_GEP(
				llvm_ctx,
				zval_addr,
				offsetof(zval, value.zv),
				PointerType::getUnqual(
						llvm_ctx.zval_ptr_type)), 4);
}
/* }}} */

/* {{{ static Value* zend_jit_load_res */
static Value* zend_jit_load_res(zend_llvm_ctx &llvm_ctx,
                                Value         *zval_addr,
                                int            ssa_var,
                                uint32_t       info)
{
	if (info & MAY_BE_IN_REG) {
		return llvm_ctx.builder.CreateAlignedLoad(llvm_ctx.reg[ssa_var], 4);
	}
	return llvm_ctx.builder.CreateAlignedLoad(
			zend_jit_GEP(
				llvm_ctx,
				zval_addr,
				offsetof(zval, value.res),
				PointerType::getUnqual(
					PointerType::getUnqual(
						llvm_ctx.zend_res_type))), 4);
}
/* }}} */

/* {{{ static Value* zend_jit_load_res_handle */
static Value* zend_jit_load_res_handle(zend_llvm_ctx &llvm_ctx,
                                       Value         *res_addr)
{
	return llvm_ctx.builder.CreateAlignedLoad(
			zend_jit_GEP(
				llvm_ctx,
				res_addr,
				offsetof(zend_resource, handle),
				PointerType::getUnqual(
					Type::getInt32Ty(llvm_ctx.context))), 4);
}
/* }}} */

/* {{{ static Value* zend_jit_load_array */
static Value* zend_jit_load_array(zend_llvm_ctx &llvm_ctx,
                                  Value         *zval_addr,
                                  int            ssa_var,
                                  uint32_t       info)
{
	if (info & MAY_BE_IN_REG) {
		return llvm_ctx.builder.CreateAlignedLoad(llvm_ctx.reg[ssa_var], 4);
	}
	return llvm_ctx.builder.CreateAlignedLoad(
			zend_jit_GEP(
				llvm_ctx,
				zval_addr,
				offsetof(zval, value.arr),
				PointerType::getUnqual(
					PointerType::getUnqual(
						llvm_ctx.zend_array_type))), 4);
}
/* }}} */

/* {{{ static Value* zend_jit_load_array_ht */
static Value* zend_jit_load_array_ht(zend_llvm_ctx &llvm_ctx,
                                     Value         *arr_addr)
{
	return zend_jit_GEP(
		llvm_ctx,
		arr_addr,
		0,
		PointerType::getUnqual(llvm_ctx.HashTable_type));
//	return llvm_ctx.builder.CreateBitCast(
//		arr_addr,
//		PointerType::getUnqual(llvm_ctx.HashTable_type));
}
/* }}} */

/* {{{ static Value* zend_jit_load_str */
static Value* zend_jit_load_str(zend_llvm_ctx &llvm_ctx,
                                Value         *zval_addr,
                                int            ssa_var,
                                uint32_t       info)
{
	if (info & MAY_BE_IN_REG) {
		return llvm_ctx.builder.CreateAlignedLoad(llvm_ctx.reg[ssa_var], 4);
	}
	return llvm_ctx.builder.CreateAlignedLoad(
			zend_jit_GEP(
				llvm_ctx,
				zval_addr,
				offsetof(zval, value.str),
				PointerType::getUnqual(
					PointerType::getUnqual(
						llvm_ctx.zend_string_type))), 4);
}
/* }}} */

/* {{{ static Value* zend_jit_load_str_val */
static Value* zend_jit_load_str_val(zend_llvm_ctx &llvm_ctx,
                                    Value         *str)
{
	return zend_jit_GEP(
			llvm_ctx,
			str,
			offsetof(zend_string, val),
			PointerType::getUnqual(
				Type::getInt8Ty(llvm_ctx.context)));
}
/* }}} */

/* {{{ static Value* zend_jit_load_str_len */
static Value* zend_jit_load_str_len(zend_llvm_ctx &llvm_ctx,
                                    Value         *str)
{
	return llvm_ctx.builder.CreateAlignedLoad(
			zend_jit_GEP(
				llvm_ctx,
				str,
				offsetof(zend_string, len),
				PointerType::getUnqual(
					LLVM_GET_LONG_TY(llvm_ctx.context))), 4);
}
/* }}} */

/* {{{ static Value* zend_jit_load_obj */
static Value* zend_jit_load_obj(zend_llvm_ctx &llvm_ctx,
                                Value         *zval_addr,
                                int            ssa_var,
                                uint32_t       info)
{
	if (info & MAY_BE_IN_REG) {
		return llvm_ctx.builder.CreateAlignedLoad(llvm_ctx.reg[ssa_var], 4);
	}
	return llvm_ctx.builder.CreateAlignedLoad(
			zend_jit_GEP(
				llvm_ctx,
				zval_addr,
				offsetof(zval, value.obj),
				PointerType::getUnqual(
					PointerType::getUnqual(
						llvm_ctx.zend_object_type))), 4);
}
/* }}} */

/* {{{ static Value* zend_jit_load_obj_handlers */
static Value* zend_jit_load_obj_handlers(zend_llvm_ctx &llvm_ctx,
                                         Value         *obj_addr)
{
	return llvm_ctx.builder.CreateAlignedLoad(
			zend_jit_GEP(
				llvm_ctx,
				obj_addr,
				offsetof(zend_object, handlers),
				PointerType::getUnqual(
					PointerType::getUnqual(
						Type::LLVM_GET_LONG_TY(llvm_ctx.context)))), 4);
}
/* }}} */

/* {{{ static Value* zend_jit_load_obj_handle */
static Value* zend_jit_load_obj_handle(zend_llvm_ctx &llvm_ctx,
                                       Value         *obj_addr)
{
	return llvm_ctx.builder.CreateAlignedLoad(
			zend_jit_GEP(
				llvm_ctx,
				obj_addr,
				offsetof(zend_object, handle),
					PointerType::getUnqual(
						Type::getInt32Ty(llvm_ctx.context))), 4);
}
/* }}} */

/* {{{ static Value* zend_jit_load_obj_ce */
static Value* zend_jit_load_obj_ce(zend_llvm_ctx &llvm_ctx,
                                   Value         *obj_addr)
{
	return llvm_ctx.builder.CreateAlignedLoad(
			zend_jit_GEP(
				llvm_ctx,
				obj_addr,
				offsetof(zend_object, ce),
					PointerType::getUnqual(
						PointerType::getUnqual(
							llvm_ctx.zend_class_entry_type))), 4);
}
/* }}} */

/* {{{ static Value* zend_jit_load_ce_name */
static Value* zend_jit_load_ce_name(zend_llvm_ctx &llvm_ctx,
                                   Value         *class_entry)
{
	return llvm_ctx.builder.CreateAlignedLoad(
			zend_jit_GEP(
				llvm_ctx,
				class_entry,
				offsetof(zend_class_entry, name),
					PointerType::getUnqual(
						PointerType::getUnqual(
							llvm_ctx.zend_string_type))), 4);
}
/* }}} */

/* {{{ static int zend_jit_try_addref */
static int zend_jit_try_addref(zend_llvm_ctx    &llvm_ctx,
                               Value            *val,
                               Value            *type_info,
                               zend_uchar        op_type,
                               znode_op         *op,
                               int               ssa_var,
                               uint32_t          info)
{
	if (!(info & (MAY_BE_ANY - (MAY_BE_OBJECT|MAY_BE_RESOURCE)))) {
		zend_jit_addref(llvm_ctx,
			zend_jit_load_counted(llvm_ctx, val, ssa_var, info));
	} else if (info & (MAY_BE_STRING|MAY_BE_ARRAY|MAY_BE_OBJECT|MAY_BE_RESOURCE)) {
		BasicBlock *bb_rc = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_norc = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		if (!type_info) {
			type_info = zend_jit_load_type_info_c(llvm_ctx, val, op_type, op, ssa_var, info);
		}
		zend_jit_expected_br(llvm_ctx,
			llvm_ctx.builder.CreateICmpNE(
				llvm_ctx.builder.CreateAnd(
					type_info,
					llvm_ctx.builder.getInt32(IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT)),
			llvm_ctx.builder.getInt32(0)),
			bb_rc,
			bb_norc);
		llvm_ctx.builder.SetInsertPoint(bb_rc);
		zend_jit_addref(llvm_ctx,
			zend_jit_load_counted(llvm_ctx, val, ssa_var, info));
		llvm_ctx.builder.CreateBr(bb_norc);
		llvm_ctx.builder.SetInsertPoint(bb_norc);
	}
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_zval_copy_ctor */
static int zend_jit_zval_copy_ctor(zend_llvm_ctx &llvm_ctx,
                                   Value         *zval_addr,
                                   Value         *type_info,
                                   zend_uchar     op_type,
                                   znode_op      *op,
                                   int            ssa_var,
                                   uint32_t       info,
                                   zend_op       *opline)
{
	if (!type_info) {
		type_info = zend_jit_load_type_info_c(llvm_ctx, zval_addr, op_type, op, ssa_var, info);
	}

	if (info & (MAY_BE_STRING|MAY_BE_ARRAY|MAY_BE_OBJECT|MAY_BE_RESOURCE|MAY_BE_REF)) {
		BasicBlock *bb_end = NULL;

		if (info & (MAY_BE_ANY - (MAY_BE_ARRAY|MAY_BE_OBJECT|MAY_BE_RESOURCE))) {
			//JIT: if (Z_OPT_REFCOUNTED_P(zvalue) || Z_OPT_IMMUTABLE_P(zvalue)) {
			BasicBlock *bb_copy = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_end = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			zend_jit_unexpected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpNE(
					llvm_ctx.builder.CreateAnd(
						type_info,
						llvm_ctx.builder.getInt32((IS_TYPE_IMMUTABLE | IS_TYPE_REFCOUNTED) << Z_TYPE_FLAGS_SHIFT)),
					llvm_ctx.builder.getInt32(0)),
				bb_copy,
				bb_end);
			llvm_ctx.builder.SetInsertPoint(bb_copy);
		}

		if (info & (MAY_BE_STRING|MAY_BE_ARRAY)) {
			BasicBlock *bb_no_copy = NULL;
			if (info & (MAY_BE_ANY - (MAY_BE_STRING|MAY_BE_ARRAY))) {
				//JIT: if (Z_OPT_IMMUTABLE_P(var_ptr) || Z_OPT_COPYABLE_P(var_ptr)) {
				BasicBlock *bb_copy = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				bb_no_copy = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				zend_jit_unexpected_br(llvm_ctx,
					llvm_ctx.builder.CreateICmpNE(
						llvm_ctx.builder.CreateAnd(
							type_info,
							llvm_ctx.builder.getInt32((IS_TYPE_IMMUTABLE | IS_TYPE_COPYABLE) << Z_TYPE_FLAGS_SHIFT)),
					llvm_ctx.builder.getInt32(0)),
					bb_copy,
					bb_no_copy);
				llvm_ctx.builder.SetInsertPoint(bb_copy);
			}

			//JIT: zval_copy_ctor_func(var_ptr);
			zend_jit_copy_ctor_func(llvm_ctx, zval_addr, opline->lineno);
			if (!bb_end) {
				bb_end = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(bb_end);
			if (bb_no_copy) {
				llvm_ctx.builder.SetInsertPoint(bb_no_copy);
			}
		}

		//JIT: if (Z_OPT_REFCOUNTED_P(var_ptr)) Z_ADDREF_P(var_ptr);
		zend_jit_try_addref(llvm_ctx, zval_addr, type_info, op_type, op, ssa_var, info);

		if (bb_end) {
			llvm_ctx.builder.CreateBr(bb_end);
			llvm_ctx.builder.SetInsertPoint(bb_end);
		}
	}

	return 1;
}
/* }}} */

/* {{{ static int zend_jit_separate_zval_noref */
static int zend_jit_separate_zval_noref(zend_llvm_ctx &llvm_ctx,
                                        Value         *zval_addr,
                                        Value         *type_info,
                                        zend_uchar     op_type,
                                        znode_op      *op,
                                        int            ssa_var,
                                        uint32_t       info,
                                        zend_op       *opline)
{

//???	if ((info & (MAY_BE_STRING|MAY_BE_ARRAY)) && (info && MAY_BE_RCN))
	if ((info & (MAY_BE_STRING|MAY_BE_ARRAY))) {
		BasicBlock *bb_end = NULL;

		if (info & (MAY_BE_ANY - MAY_BE_ARRAY)) {
			//JIT: if (Z_COPYABLE_P(_zv) || Z_IMMUTABLE_P(_zv))
			BasicBlock *bb_copy = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			if (!bb_end) {
				bb_end = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}

			if (!type_info) {
				type_info = zend_jit_load_type_info_c(llvm_ctx, zval_addr, op_type, op, ssa_var, info);
			}

			zend_jit_unexpected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpNE(
					llvm_ctx.builder.CreateAnd(
						type_info,
						llvm_ctx.builder.getInt32((IS_TYPE_IMMUTABLE | IS_TYPE_COPYABLE) << Z_TYPE_FLAGS_SHIFT)),
					llvm_ctx.builder.getInt32(0)),
				bb_copy,
				bb_end);
			llvm_ctx.builder.SetInsertPoint(bb_copy);
		}

		Value *refcount_addr = NULL;
		Value *refcount = NULL;
		
//???		if (info && MAY_BE_RC1)
		//JIT: if (Z_REFCOUNT_P(_zv) > 1) {
		BasicBlock *bb_copy2 = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		if (!bb_end) {
			bb_end = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		}
		refcount_addr = zend_jit_refcount_addr(llvm_ctx,
			zend_jit_load_counted(llvm_ctx, zval_addr, ssa_var, info));
		refcount = llvm_ctx.builder.CreateAlignedLoad(refcount_addr, 4);
		zend_jit_unexpected_br(llvm_ctx,
			llvm_ctx.builder.CreateICmpUGT(
				refcount,
				llvm_ctx.builder.getInt32(1)),
			bb_copy2,
			bb_end);
		llvm_ctx.builder.SetInsertPoint(bb_copy2);
//???		}

		BasicBlock *bb_copy3 = NULL;

		if (info & MAY_BE_ARRAY) {
			//JIT: if (!Z_IMMUTABLE_P(_zv))
			BasicBlock *bb_rc = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_copy3 = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			if (!type_info) {
				type_info = zend_jit_load_type_info_c(llvm_ctx, zval_addr, op_type, op, ssa_var, info);
			}
			zend_jit_expected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(
					llvm_ctx.builder.CreateAnd(
						type_info,
						llvm_ctx.builder.getInt32(IS_TYPE_IMMUTABLE << Z_TYPE_FLAGS_SHIFT)),
				llvm_ctx.builder.getInt32(0)),
			bb_rc,
			bb_copy3);
			llvm_ctx.builder.SetInsertPoint(bb_rc);
		}

		//JIT: Z_DELREF_P(_zv);
		llvm_ctx.builder.CreateAlignedStore(
			llvm_ctx.builder.CreateSub(
				refcount,
				llvm_ctx.builder.getInt32(1)),
			refcount_addr, 4);

		if (bb_copy3) {
			llvm_ctx.builder.CreateBr(bb_copy3);
			llvm_ctx.builder.SetInsertPoint(bb_copy3);
		}
		//JIT: zval_copy_ctor_func(_zv);
		zend_jit_copy_ctor_func(llvm_ctx, zval_addr, opline->lineno);

		if (bb_end) {
			llvm_ctx.builder.CreateBr(bb_end);
			llvm_ctx.builder.SetInsertPoint(bb_end);
		}
	}
	
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_separate_array */
static int zend_jit_separate_array(zend_llvm_ctx  &llvm_ctx,
                                   Value          *zval_addr,
                                   Value          *type_info,
                                   zend_uchar      op_type,
                                   znode_op       *op,
                                   int             ssa_var,
                                   uint32_t        info,
                                   zend_op        *opline)
{
	Value *refcount_addr = NULL;
	Value *refcount = NULL;

	if (info && MAY_BE_RCN) {
		//JIT: if (Z_REFCOUNT_P(_zv) > 1) {
		BasicBlock *bb_cont = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_skip = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

		refcount_addr = zend_jit_refcount_addr(
				llvm_ctx, 
				zend_jit_load_array(llvm_ctx, zval_addr, ssa_var, info));

		refcount = llvm_ctx.builder.CreateAlignedLoad(refcount_addr, 4);
		zend_jit_unexpected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpUGT(
					refcount,
					llvm_ctx.builder.getInt32(1)),
				bb_cont,
				bb_skip);
		llvm_ctx.builder.SetInsertPoint(bb_cont);

		if (!type_info) {
			type_info = zend_jit_load_type_info_c(llvm_ctx, zval_addr, op_type, op, ssa_var, info);
		}

		//JIT: if (!Z_IMMUTABLE_P(_zv))
		BasicBlock *bb_delref = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_copy = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		zend_jit_expected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(
					llvm_ctx.builder.CreateAnd(
						type_info,
						llvm_ctx.builder.getInt32(IS_TYPE_IMMUTABLE << Z_TYPE_FLAGS_SHIFT)),
					llvm_ctx.builder.getInt32(0)),
				bb_delref,
				bb_copy);
		llvm_ctx.builder.SetInsertPoint(bb_delref);

		llvm_ctx.builder.CreateAlignedStore(
			llvm_ctx.builder.CreateSub(
				refcount,
				llvm_ctx.builder.getInt32(1)),
			refcount_addr, 4);
		llvm_ctx.builder.CreateBr(bb_copy);
		llvm_ctx.builder.SetInsertPoint(bb_copy);
		//JIT: zval_copy_ctor_func(_zv);
		zend_jit_copy_ctor_func(llvm_ctx, zval_addr, opline->lineno);

		llvm_ctx.builder.CreateBr(bb_skip);
		llvm_ctx.builder.SetInsertPoint(bb_skip);
	}
}
/* }}} */

static int zend_jit_undef_cv(zend_llvm_ctx &llvm_ctx,
                             uint32_t       var,
                             zend_op       *opline) /* {{{ */
{
	// JIT: zend_error(E_NOTICE, "Undefined variable: %s", cv->val);
	if (!llvm_ctx.valid_opline) {
		// Store "opline" in EX(opline) for error messages etc
		JIT_CHECK(zend_jit_store_opline(llvm_ctx, opline, false));
	}
	zend_jit_error(llvm_ctx, opline, E_NOTICE, "Undefined variable: %s",
		LLVM_GET_CONST_STRING(llvm_ctx.op_array->vars[EX_VAR_TO_NUM(var)]->val));

	return 1;
}
/* }}} */

/* {{{ static Value* zend_jit_load_cv */
static Value* zend_jit_load_cv(zend_llvm_ctx &llvm_ctx,
                               uint32_t       var,
                               uint32_t       info,
                               int            ssa_var,
                               int            check,
                               zend_op       *opline,
                               uint32_t       mode = BP_VAR_R)
{
	Value *zval_addr = NULL;
	PHI_DCL(cv, 2);

	// JIT: ret = EX_VAR(var)
	if ((info & MAY_BE_DEF) || mode == BP_VAR_RW || mode == BP_VAR_W) {
		zval_addr = zend_jit_load_cv_addr(llvm_ctx, var);
	}
	if (info & MAY_BE_UNDEF) {
		BasicBlock *bb_def = NULL;

		if (info & MAY_BE_DEF && mode != BP_VAR_IS) {
			if (mode == BP_VAR_R || mode == BP_VAR_UNSET) {
				PHI_ADD(cv, zval_addr);
			}
			// JIT: UNEXPECTED(Z_TYPE_P(ret) == IS_UNDEF)
			BasicBlock *bb_undef = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_def = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			zend_jit_unexpected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(
					zend_jit_load_type(llvm_ctx, zval_addr, ssa_var, info),
					llvm_ctx.builder.getInt8(IS_UNDEF)),
				bb_undef,
				bb_def);
			llvm_ctx.builder.SetInsertPoint(bb_undef);
		}

		switch (mode) {
			case BP_VAR_R:
			case BP_VAR_UNSET: {
				// JIT: zend_error(E_NOTICE, "Undefined variable: %s", cv->val);
				JIT_CHECK(zend_jit_undef_cv(llvm_ctx, var, opline));
				if (check && !(JIT_DATA(llvm_ctx.op_array)->flags & ZEND_JIT_FUNC_NO_FRAME)) {
					JIT_CHECK(zend_jit_check_exception(llvm_ctx, opline));
				}
				Value *uninitialized = llvm_ctx._EG_uninitialized_zval;
				PHI_ADD(cv, uninitialized);
				if (bb_def) {
					llvm_ctx.builder.CreateBr(bb_def);
					llvm_ctx.builder.SetInsertPoint(bb_def);
				}
				PHI_SET(cv, zval_addr, llvm_ctx.zval_ptr_type);
				break;
			}
			case BP_VAR_IS:
				/* nothing to do */
				break;
			case BP_VAR_RW:
				// JIT: zend_error(E_NOTICE, "Undefined variable: %s", cv->val);
				JIT_CHECK(zend_jit_undef_cv(llvm_ctx, var, opline));
				if (check && !(JIT_DATA(llvm_ctx.op_array)->flags & ZEND_JIT_FUNC_NO_FRAME)) {
					JIT_CHECK(zend_jit_check_exception(llvm_ctx, opline));
				}
				/* break missing intentionally */
			case BP_VAR_W:
				zend_jit_save_zval_type_info(llvm_ctx, zval_addr, ssa_var, info, llvm_ctx.builder.getInt32(IS_NULL));
				if (bb_def) {
					llvm_ctx.builder.CreateBr(bb_def);
					llvm_ctx.builder.SetInsertPoint(bb_def);
				}
				break;
			default:
				ASSERT_NOT_REACHED();
		}
	}
	return zval_addr;
}
/* }}} */

/* {{{ static int zend_jit_needs_check_for_this */
static int zend_jit_needs_check_for_this(zend_llvm_ctx &llvm_ctx, int bb)
{
	zend_op_array *op_array = llvm_ctx.op_array;
	zend_jit_func_info *info = JIT_DATA(op_array);

	ZEND_ASSERT(bb >= 0 && bb < info->cfg.blocks);
	if (!zend_bitset_in(llvm_ctx.this_checked, bb)) {
		zend_bitset_incl(llvm_ctx.this_checked, bb);
		while (bb != info->cfg.block[bb].idom) {
			bb = info->cfg.block[bb].idom;
			if (bb < 0) {
				return 1;
			} else if (zend_bitset_in(llvm_ctx.this_checked, bb)) {
				return 0;
			}
		}
		return 1;
	}
	return 0;
}
/* }}} */

/* {{{ static Value* zend_jit_deref */
static Value* zend_jit_deref(zend_llvm_ctx &llvm_ctx,
                             Value         *zval_ptr,
                             int            ssa_var,
                             uint32_t       info)
{
	if (info & MAY_BE_REF) {
		BasicBlock *bb_ref = NULL;
		BasicBlock *bb_end = NULL;
		PHI_DCL(deref, 2)

		if (info & (MAY_BE_RC1 | MAY_BE_RCN)) {
			PHI_ADD(deref, zval_ptr);
			bb_ref = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_end = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			zend_jit_expected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(
					zend_jit_load_type(llvm_ctx, zval_ptr, ssa_var, info),
					llvm_ctx.builder.getInt8(IS_REFERENCE)),
				bb_ref,
				bb_end);
			llvm_ctx.builder.SetInsertPoint(bb_ref);
		}
		Value *counted = zend_jit_load_counted(llvm_ctx, zval_ptr, ssa_var, info);
		Value *ref = zend_jit_load_reference(llvm_ctx, counted);
		PHI_ADD(deref, ref);
		if (bb_end) {
			llvm_ctx.builder.CreateBr(bb_end);
			llvm_ctx.builder.SetInsertPoint(bb_end);
		}
		PHI_SET(deref, zval_ptr, llvm_ctx.zval_ptr_type);
	}
	return zval_ptr;
}
/* }}} */

/* {{{ static Value* zend_jit_load_operand */
static Value* zend_jit_load_operand(zend_llvm_ctx &llvm_ctx,
                                    zend_uchar     op_type,
                                    znode_op      *op,
                                    int            ssa_var,
                                    uint32_t       info,
                                    int            check,
                                    zend_op       *opline, 
                                    zend_bool      fetch_obj = 0,
                                    uint32_t       mode = BP_VAR_R)
{
	if (op_type == IS_CONST) {
		return zend_jit_load_const(llvm_ctx, RT_CONSTANT(llvm_ctx.op_array, *op));
	} else if (info & MAY_BE_IN_REG) {
		return NULL;
	} else if (op_type == IS_TMP_VAR) {
		return zend_jit_load_tmp_zval(llvm_ctx, op->var);
	} else if (op_type == IS_VAR) {
		return zend_jit_load_var(llvm_ctx, op->var); //???, ssa_var, info);
	} else if (op_type == IS_CV) {
		return zend_jit_load_cv(llvm_ctx, op->var, info, ssa_var, check, opline, mode);
	} else if (op_type == IS_UNUSED && fetch_obj) {
		Value *this_ptr =  zend_jit_GEP(
			llvm_ctx,
			llvm_ctx._execute_data,
			offsetof(zend_execute_data, This),
			llvm_ctx.zval_ptr_type);
		return this_ptr;
	} else {
		ASSERT_NOT_REACHED();
	}
}
/* }}} */

/* {{{ static int zend_jit_should_swap_operands */
static int zend_jit_should_swap_operands(zend_op_array *op_array, zend_op *opline)
{
	/* Prefer constants as the second operand.  */
	if (OP2_OP_TYPE() == IS_CONST)
		return 0;
	if (OP1_OP_TYPE() == IS_CONST)
		return 1;

	/* Prefer temp variables as the second operand, because we know we can load
	   their addresses without calling out of line.  */
	if (OP2_OP_TYPE() == IS_TMP_VAR)
		return 0;
	if (OP1_OP_TYPE() == IS_TMP_VAR)
		return 1;

	/* Finally, prefer CV's that will always be defined as the second
	   operand. */
	if (OP2_OP_TYPE() == IS_CV && !OP2_MAY_BE(MAY_BE_UNDEF))
		return 0;
	if (OP1_OP_TYPE() == IS_CV && !OP1_MAY_BE(MAY_BE_UNDEF))
		return 1;

	/* Otherwise it doesn't matter.  */
	return 0;
}
/* }}} */

#define SAME_CVs(opline) \
	(OP1_OP_TYPE() == IS_CV && \
	 OP2_OP_TYPE() == IS_CV && \
	 OP1_OP()->var == OP2_OP()->var)

/* {{{ static int zend_jit_load_operands */
static int zend_jit_load_operands(zend_llvm_ctx     &llvm_ctx,
                                  zend_op_array     *op_array,
                                  zend_op           *opline,
                                  Value            **op1_addr,
                                  Value            **op2_addr)
{
    // Check if operands are the same CV
	if (SAME_CVs(opline)) {
		*op1_addr = zend_jit_load_operand(llvm_ctx,
				OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO(), 0, opline);
		*op2_addr = *op1_addr;
		return 1;
	}

	// Select optimal operand loading order
	if (zend_jit_should_swap_operands(op_array, opline)) {
		*op2_addr = zend_jit_load_operand(llvm_ctx,
				OP2_OP_TYPE(), OP2_OP(), OP2_SSA_VAR(), OP2_INFO(), 0, opline);
		*op1_addr = zend_jit_load_operand(llvm_ctx,
				OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO(), 0, opline);
	} else {
		*op1_addr = zend_jit_load_operand(llvm_ctx,
				OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO(), 0, opline);
		*op2_addr = zend_jit_load_operand(llvm_ctx,
				OP2_OP_TYPE(), OP2_OP(), OP2_SSA_VAR(), OP2_INFO(), 0, opline);
	}
	return 1;
}
/* }}} */

/* {{{ static Value *zend_jit_reload_from_reg */
static Value *zend_jit_reload_from_reg(zend_llvm_ctx    &llvm_ctx,
                                       int               from_var,
                                       uint32_t          from_info)
{
	zend_op_array *op_array = llvm_ctx.op_array;
	zend_jit_func_info *info = JIT_DATA(op_array);
	Value *to_addr = zend_jit_load_slot(llvm_ctx, (zend_uintptr_t)ZEND_CALL_VAR_NUM(NULL, info->ssa.var[from_var].var));

	zend_jit_save_zval_type_info(llvm_ctx, to_addr, -1, MAY_BE_ANY,
		zend_jit_load_type_info(llvm_ctx, NULL, from_var, from_info));
	if (from_info & (MAY_BE_ANY-(MAY_BE_NULL|MAY_BE_FALSE|MAY_BE_TRUE))) {
		if (from_info & MAY_BE_LONG) {
			zend_jit_save_zval_lval(llvm_ctx, to_addr, -1, MAY_BE_ANY,
				zend_jit_load_lval(llvm_ctx, NULL, from_var, from_info));
		} else if (from_info & MAY_BE_DOUBLE) {
			zend_jit_save_zval_dval(llvm_ctx, to_addr, -1, MAY_BE_ANY,
				zend_jit_load_dval(llvm_ctx, NULL, from_var, from_info));
		} else {
			zend_jit_save_zval_ptr(llvm_ctx, to_addr, -1, MAY_BE_ANY,
				zend_jit_load_ptr(llvm_ctx, NULL, from_var, from_info));
		}
	}
	return to_addr;
}
/* }}} */

/* {{{ static int zend_jit_reload_to_reg */
static int zend_jit_reload_to_reg(zend_llvm_ctx    &llvm_ctx,
                                  Value            *from_addr,
                                  int               to_var,
                                  uint32_t          to_info)
{
	zend_op_array *op_array = llvm_ctx.op_array;
	zend_jit_func_info *info = JIT_DATA(op_array);

	if (to_info & (MAY_BE_NULL|MAY_BE_FALSE|MAY_BE_TRUE)) {
		zend_jit_save_zval_type_info(llvm_ctx, NULL, to_var, to_info,
			zend_jit_load_type_info(llvm_ctx, from_addr, -1, MAY_BE_ANY));
	} else if (to_info & MAY_BE_LONG) {
		zend_jit_save_zval_lval(llvm_ctx, NULL, to_var, to_info,
			zend_jit_load_lval(llvm_ctx, from_addr, -1, MAY_BE_ANY));
	} else if (to_info & MAY_BE_DOUBLE) {
		zend_jit_save_zval_dval(llvm_ctx, NULL, to_var, to_info,
			zend_jit_load_dval(llvm_ctx, from_addr, -1, MAY_BE_ANY));
	} else {
		zend_jit_save_zval_ptr(llvm_ctx, NULL, to_var, to_info,
			zend_jit_load_ptr(llvm_ctx, from_addr, -1, MAY_BE_ANY));
	}
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_load_operand_addr */
static Value* zend_jit_load_operand_addr(zend_llvm_ctx &llvm_ctx,
                                         zend_uchar     op_type,
                                         znode_op      *op,
                                         int            ssa_var,
                                         uint32_t       info,
                                         int            check,
                                         zend_op       *opline, 
                                         zend_bool      fetch_obj,
                                         uint32_t       mode,
                                         Value        **should_free)
{
	Value *zv_addr = zend_jit_load_operand(llvm_ctx,
				op_type, op, ssa_var, info, check, opline, fetch_obj, mode);

	if (op_type == IS_VAR) {
		if (!zv_addr) {
			*should_free = NULL;
			return NULL;
		}
		PHI_DCL(zv_addr, 3);
		PHI_DCL(to_free, 3);
		BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_next = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_common = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		Value *zv_type = zend_jit_load_type_info(llvm_ctx, zv_addr, ssa_var, MAY_BE_ANY);
		Value *to_free;

		//JIT: if (EXPECTED(Z_TYPE_P(ret) == IS_INDIRECT)) {
		zend_jit_expected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(
					zv_type,
					llvm_ctx.builder.getInt32(IS_INDIRECT)),
				bb_follow,
				bb_next);
		llvm_ctx.builder.SetInsertPoint(bb_follow);
		//JIT: should_free->var = NULL;
		to_free = llvm_ctx.builder.CreateIntToPtr(
			LLVM_GET_LONG(0), llvm_ctx.zval_ptr_type);
		//JIT: return Z_INDIRECT_P(ret);
		Value *tmp = zend_jit_load_ind(llvm_ctx, zv_addr);
		PHI_ADD(to_free, to_free);
		PHI_ADD(zv_addr, tmp);
		llvm_ctx.builder.CreateBr(bb_common);

		//JIT: } else if (!Z_REFCOUNTED_P(ret) || Z_REFCOUNT_P(ret) == 1) {
		llvm_ctx.builder.SetInsertPoint(bb_next);
		bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		bb_next = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		zend_jit_unexpected_br(llvm_ctx,
			llvm_ctx.builder.CreateICmpEQ(
				llvm_ctx.builder.CreateAnd(
					zv_type,
					llvm_ctx.builder.getInt32(IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT)),
				llvm_ctx.builder.getInt32(0)),
				bb_follow,
				bb_next);
		llvm_ctx.builder.SetInsertPoint(bb_next);
		bb_next = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		Value *counted = zend_jit_load_counted(llvm_ctx, zv_addr, ssa_var, info);
		zend_jit_unexpected_br(llvm_ctx,				
			llvm_ctx.builder.CreateICmpEQ(
				llvm_ctx.builder.CreateAlignedLoad(
					zend_jit_refcount_addr(llvm_ctx, counted), 4),
				llvm_ctx.builder.getInt32(1)),
			bb_follow,
			bb_next);

		llvm_ctx.builder.SetInsertPoint(bb_follow);
		//JIT: should_free->var = ret;
		//JIT: return ret;
		PHI_ADD(to_free, zv_addr);
		PHI_ADD(zv_addr, zv_addr);
		llvm_ctx.builder.CreateBr(bb_common);
		
		//JIT: } else {
		llvm_ctx.builder.SetInsertPoint(bb_next);

		//JIT: Z_DELREF_P(ret);
		zend_jit_delref(llvm_ctx, counted);
		//JIT: should_free->var = NULL;
		to_free = llvm_ctx.builder.CreateIntToPtr(
			LLVM_GET_LONG(0), llvm_ctx.zval_ptr_type);
		//JIT: return ret;
		PHI_ADD(to_free, to_free);
		PHI_ADD(zv_addr, zv_addr);
		llvm_ctx.builder.CreateBr(bb_common);

		llvm_ctx.builder.SetInsertPoint(bb_common);
		PHI_SET(to_free, to_free, llvm_ctx.zval_ptr_type);
		PHI_SET(zv_addr, zv_addr, llvm_ctx.zval_ptr_type);
		*should_free = to_free;
	}

	return zv_addr;
}
/* }}} */

/* {{{ static int zend_jit_copy_value */
static int zend_jit_copy_value(zend_llvm_ctx &llvm_ctx,
                               Value         *to_addr,
                               uint32_t       old_info,
                               int            to_ssa_var,
                               uint32_t       to_info,
                               Value         *from_addr,
                               Value         *from_type,
                               zend_uchar     from_op_type,
                               znode_op       *from_op,
                               int            from_ssa_var,
                               uint32_t       from_info)
{
	// FIXME: don't store type if it is not necessary
	// FIXME: use immediate value if type if it's exactly known
	if (!from_type) {
		from_type = zend_jit_load_type_info_c(llvm_ctx, from_addr, from_op_type, from_op, from_ssa_var, from_info);
	}

	if (from_info & (MAY_BE_ANY - (MAY_BE_NULL|MAY_BE_FALSE|MAY_BE_TRUE))) {
		if (from_op_type == IS_CONST) {
			if (Z_TYPE_P(RT_CONSTANT(llvm_ctx.op_array, *from_op)) == IS_DOUBLE) {
#if SIZEOF_ZEND_LONG == 8
				if (to_info & MAY_BE_IN_REG) {
					zend_jit_save_zval_dval(llvm_ctx, to_addr, to_ssa_var, to_info,
						zend_jit_load_dval(llvm_ctx, from_addr, from_ssa_var, from_info));
				} else {
					zend_jit_save_zval_lval(llvm_ctx, to_addr, to_ssa_var, to_info,
						LLVM_GET_LONG(Z_LVAL_P(RT_CONSTANT(llvm_ctx.op_array, *from_op))));
				}
#else
				zend_jit_save_zval_dval(llvm_ctx, to_addr, to_ssa_var, to_info,
					zend_jit_load_dval(llvm_ctx, from_addr, from_ssa_var, from_info));
#endif
			} else if ((to_info & MAY_BE_IN_REG) || (from_info & MAY_BE_IN_REG)) {
				if (from_info & MAY_BE_LONG) {
					if ((to_info & (MAY_BE_LONG|MAY_BE_DOUBLE)) == MAY_BE_DOUBLE) {
						zend_jit_save_zval_dval(llvm_ctx, to_addr, to_ssa_var, to_info,
							llvm_ctx.builder.CreateSIToFP(
								LLVM_GET_LONG(Z_LVAL_P(RT_CONSTANT(llvm_ctx.op_array, *from_op))),
								Type::getDoubleTy(llvm_ctx.context)));
					} else {
						zend_jit_save_zval_lval(llvm_ctx, to_addr, to_ssa_var, to_info,
							LLVM_GET_LONG(Z_LVAL_P(RT_CONSTANT(llvm_ctx.op_array, *from_op))));
					}
				} else {
					zend_jit_save_zval_ptr(llvm_ctx, to_addr, to_ssa_var, to_info,
						zend_jit_load_ptr(llvm_ctx, from_addr, from_ssa_var, from_info));
				}
			} else {
				if ((to_info & (MAY_BE_LONG|MAY_BE_DOUBLE)) == MAY_BE_DOUBLE) {
					zend_jit_save_zval_dval(llvm_ctx, to_addr, to_ssa_var, to_info,
						llvm_ctx.builder.CreateSIToFP(
							LLVM_GET_LONG(Z_LVAL_P(RT_CONSTANT(llvm_ctx.op_array, *from_op))),
							Type::getDoubleTy(llvm_ctx.context)));
				} else {
					zend_jit_save_zval_lval(llvm_ctx, to_addr, to_ssa_var, to_info,
						LLVM_GET_LONG(Z_LVAL_P(RT_CONSTANT(llvm_ctx.op_array, *from_op))));
				}
			}
		} else {
			if (from_info & MAY_BE_DOUBLE) {
				if (from_info & (MAY_BE_ANY-MAY_BE_DOUBLE)) {
#if SIZEOF_ZEND_LONG == 8
					llvm_ctx.builder.CreateAlignedStore(
						llvm_ctx.builder.CreateAlignedLoad(
							zend_jit_GEP(
								llvm_ctx,
								from_addr,
								offsetof(zval,value),
								PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4),
						zend_jit_GEP(
							llvm_ctx,
							to_addr,
							offsetof(zval,value),
							PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4);
#else
					llvm_ctx.builder.CreateAlignedStore(
						llvm_ctx.builder.CreateAlignedLoad(
							zend_jit_GEP(
								llvm_ctx,
								from_addr,
								offsetof(zval,value),
								PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4),
						zend_jit_GEP(
							llvm_ctx,
							to_addr,
							offsetof(zval,value),
							PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4);
					llvm_ctx.builder.CreateAlignedStore(
						llvm_ctx.builder.CreateAlignedLoad(
							zend_jit_GEP(
								llvm_ctx,
								from_addr,
								offsetof(zval,value) + sizeof(long),
								PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4),
						zend_jit_GEP(
							llvm_ctx,
							to_addr,
							offsetof(zval,value) + sizeof(long),
							PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4);
#endif
				} else {
					zend_jit_save_zval_dval(llvm_ctx, to_addr, to_ssa_var, to_info,
						zend_jit_load_dval(llvm_ctx, from_addr, from_ssa_var, from_info));
				}			
			} else if ((to_info & MAY_BE_IN_REG) || (from_info & MAY_BE_IN_REG)) {
				if (from_info & MAY_BE_LONG) {
					zend_jit_save_zval_lval(llvm_ctx, to_addr, to_ssa_var, to_info,
						zend_jit_load_lval_c(llvm_ctx, from_addr, from_op_type, from_op, from_ssa_var, from_info));
				} else {
					zend_jit_save_zval_ptr(llvm_ctx, to_addr, to_ssa_var, to_info,
						zend_jit_load_ptr(llvm_ctx, from_addr, from_ssa_var, from_info));
				}
			} else {
				zend_jit_save_zval_lval(llvm_ctx, to_addr, to_ssa_var, to_info,
					zend_jit_load_lval_c(llvm_ctx, from_addr, from_op_type, from_op, from_ssa_var, from_info));
			}
		}
	}

	if (!old_info ||
	    !has_concrete_type(from_info) ||
	    (from_info & (MAY_BE_STRING|MAY_BE_ARRAY)) ||
		(from_info & MAY_BE_ANY) != (old_info & MAY_BE_ANY)) {
		zend_jit_save_zval_type_info(llvm_ctx, to_addr, to_ssa_var, to_info, from_type);
	}

	return 1;
}
/* }}} */

/* {{{ static int zend_jit_make_ref */
static int zend_jit_make_ref(zend_llvm_ctx &llvm_ctx,
                             Value         *zval_addr,
                             Value         *zval_type,
                             int            ssa_var,
                             uint32_t       info)
{
	BasicBlock *bb_finish = NULL;
	if (info & (MAY_BE_REF)) {
		BasicBlock *bb_not_ref = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		if (!zval_type) {
			zval_type = zend_jit_load_type_info(llvm_ctx, zval_addr, ssa_var, info);
		}

		zend_jit_unexpected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(
					zval_type,
					llvm_ctx.builder.getInt32(IS_REFERENCE_EX)),
				bb_finish,
				bb_not_ref);
		llvm_ctx.builder.SetInsertPoint(bb_not_ref);
	}

	zend_jit_new_ref(llvm_ctx, zval_addr, zval_addr);

	if (bb_finish) {
		llvm_ctx.builder.CreateBr(bb_finish);
		llvm_ctx.builder.SetInsertPoint(bb_finish);
	}
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_zval_dtor_ex */
static int zend_jit_zval_dtor_ex(zend_llvm_ctx &llvm_ctx,
                                 Value         *zval_addr,
                                 Value         *zval_type,
                                 int            ssa_var,
                                 uint32_t       info,
                                 uint32_t       lineno)
{
	BasicBlock *bb_follow;
	BasicBlock *bb_finish;
	Value *counted;
	Value *refcount;

	if (zval_addr && (info & (MAY_BE_STRING|MAY_BE_ARRAY|MAY_BE_OBJECT|MAY_BE_RESOURCE))) {
		if (info & (MAY_BE_ANY - (MAY_BE_OBJECT | MAY_BE_RESOURCE))) {
			bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			zend_jit_expected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(
					llvm_ctx.builder.CreateAnd(
						zend_jit_load_type_flags(llvm_ctx, zval_addr, ssa_var, info),
						llvm_ctx.builder.getInt8(IS_TYPE_REFCOUNTED)),
					llvm_ctx.builder.getInt8(0)),
				bb_finish,
				bb_follow);
			llvm_ctx.builder.SetInsertPoint(bb_follow);			
		}
		counted = zend_jit_load_counted(llvm_ctx, zval_addr, ssa_var, info);
		zend_jit_zval_dtor_func(llvm_ctx, counted, lineno);		
		if (bb_finish) {
			llvm_ctx.builder.CreateBr(bb_finish);
			llvm_ctx.builder.SetInsertPoint(bb_finish);
		}
	}
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_cobject_release */
static int zend_jit_object_release(zend_llvm_ctx &llvm_ctx,
                                   Value         *object,
                                   uint32_t       lineno)
{
	BasicBlock *bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	BasicBlock *bb_gc = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	Value *counted =
		llvm_ctx.builder.CreateBitCast(
			object,
			PointerType::getUnqual(llvm_ctx.zend_refcounted_type));

	Value *refcount = zend_jit_delref(llvm_ctx, counted);
	zend_jit_expected_br(llvm_ctx,
		llvm_ctx.builder.CreateICmpEQ(
			refcount,
			llvm_ctx.builder.getInt32(0)),
		bb_follow,
		bb_gc);
	llvm_ctx.builder.SetInsertPoint(bb_follow);
	zend_jit_zval_dtor_func_for_ptr(llvm_ctx,
		counted,
		lineno);
	llvm_ctx.builder.CreateBr(bb_finish);
	llvm_ctx.builder.SetInsertPoint(bb_gc);
	//JIT: if (UNEXPECTED(!Z_GC_INFO_P(zv))) {
	bb_gc = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	zend_jit_expected_br(llvm_ctx,
		llvm_ctx.builder.CreateICmpEQ(
			llvm_ctx.builder.CreateAlignedLoad(
				zend_jit_GEP(
					llvm_ctx,
					counted,
					offsetof(zend_refcounted, gc.u.v.gc_info),
					PointerType::getUnqual(Type::getInt16Ty(llvm_ctx.context))), 2),
			llvm_ctx.builder.getInt16(0)),
		bb_gc,
		bb_finish);
	llvm_ctx.builder.SetInsertPoint(bb_gc);
	//JIT: gc_possible_root(Z_COUNTED_P(zv))
	zend_jit_gc_possible_root(llvm_ctx, counted);
	llvm_ctx.builder.CreateBr(bb_finish);
	llvm_ctx.builder.SetInsertPoint(bb_finish);
}
/* }}} */

/* {{{ static int zend_jit_zval_ptr_dtor_ex */
static int zend_jit_zval_ptr_dtor_ex(zend_llvm_ctx &llvm_ctx,
                                     Value         *zval_addr,
                                     Value         *zval_type,
                                     int            ssa_var,
                                     uint32_t       info,
                                     uint32_t       lineno,
                                     bool           check_gc)
{
	BasicBlock *bb_follow;
	BasicBlock *bb_finish = NULL;
	Value *counted;
	Value *refcount;

	if (zval_addr && (info & (MAY_BE_STRING|MAY_BE_ARRAY|MAY_BE_OBJECT|MAY_BE_RESOURCE|MAY_BE_REF))) {
		if (info & (MAY_BE_ANY - (MAY_BE_OBJECT | MAY_BE_RESOURCE))) {
			bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			zend_jit_expected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(
					llvm_ctx.builder.CreateAnd(
						zend_jit_load_type_flags(llvm_ctx, zval_addr, ssa_var, info),
						llvm_ctx.builder.getInt8(IS_TYPE_REFCOUNTED)),
					llvm_ctx.builder.getInt8(0)),
				bb_finish,
				bb_follow);
			llvm_ctx.builder.SetInsertPoint(bb_follow);			
		}

		counted = zend_jit_load_counted(llvm_ctx, zval_addr, ssa_var, info);
		refcount = zend_jit_delref(llvm_ctx, counted);
		if (!bb_finish) {
			bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		}
		bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_gc = NULL;

		check_gc = check_gc && (info & (MAY_BE_OBJECT|MAY_BE_ARRAY));
		if (check_gc) {
			bb_gc = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		} else {
			bb_gc = bb_finish;
		}
		zend_jit_expected_br(llvm_ctx,
			llvm_ctx.builder.CreateICmpEQ(
				refcount,
				llvm_ctx.builder.getInt32(0)),
			bb_follow,
			bb_gc);
		llvm_ctx.builder.SetInsertPoint(bb_follow);
		zend_jit_zval_dtor_func_for_ptr(llvm_ctx, counted, lineno);		
		llvm_ctx.builder.CreateBr(bb_finish);
		if (check_gc) {
			llvm_ctx.builder.SetInsertPoint(bb_gc);
			if (info & MAY_BE_REF) {
				//JIT: ZVAL_DEREF(z);
				zval_addr = zend_jit_deref(llvm_ctx, zval_addr, ssa_var, info);
			}
			if (info & (MAY_BE_ANY - MAY_BE_OBJECT)) {
				//JIT: if (Z_COLLECTABLE_P(z)) {
				bb_gc = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				zend_jit_expected_br(llvm_ctx,
					llvm_ctx.builder.CreateICmpNE(
						llvm_ctx.builder.CreateAnd(
							zend_jit_load_type_flags(llvm_ctx, zval_addr, ssa_var, info),
							llvm_ctx.builder.getInt8(IS_TYPE_COLLECTABLE)),
						llvm_ctx.builder.getInt8(0)),
					bb_gc,
					bb_finish);
				llvm_ctx.builder.SetInsertPoint(bb_gc);
			}
			if (info & MAY_BE_REF) {
				// reload counted
				counted = zend_jit_load_counted(llvm_ctx, zval_addr, ssa_var, info);
			}
			//JIT: if (UNEXPECTED(!Z_GC_INFO_P(zv))) {
			bb_gc = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			zend_jit_expected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(
					llvm_ctx.builder.CreateAlignedLoad(
						zend_jit_GEP(
							llvm_ctx,
							counted,
							offsetof(zend_refcounted, gc.u.v.gc_info),
							PointerType::getUnqual(Type::getInt16Ty(llvm_ctx.context))), 2),
					llvm_ctx.builder.getInt16(0)),
				bb_gc,
				bb_finish);
			llvm_ctx.builder.SetInsertPoint(bb_gc);
			//JIT: gc_possible_root(Z_COUNTED_P(zv))
			zend_jit_gc_possible_root(llvm_ctx, counted);
			llvm_ctx.builder.CreateBr(bb_finish);
		}
		llvm_ctx.builder.SetInsertPoint(bb_finish);
	}

	return 1;
}
/* }}} */

/* {{{ static int zend_jit_free_operand */
static int zend_jit_free_operand(zend_llvm_ctx &llvm_ctx,
                                 zend_uchar     op_type,
                                 Value         *zval_addr,
                                 Value         *zval_type,
                                 int            ssa_var,
                                 uint32_t       info,
                                 uint32_t       lineno,
                                 // FIXME: it may be not safe to avoid GC check
                                 bool           check_gc = false)
{
	if (!(info & MAY_BE_IN_REG) && (op_type == IS_VAR || op_type == IS_TMP_VAR)) {
		return zend_jit_zval_ptr_dtor_ex(llvm_ctx, zval_addr, zval_type, ssa_var, info, lineno, check_gc);
	}
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_free_var_ptr */
static int zend_jit_free_var_ptr(zend_llvm_ctx &llvm_ctx,
                                 Value         *should_free,
                                 int            ssa_var,
                                 uint32_t       info,
                                 zend_op       *opline)
{
	BasicBlock *bb_free = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	BasicBlock *bb_skip = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	//JIT: if (UNEXPECTED(varptr == NULL)) {
	zend_jit_unexpected_br(llvm_ctx,
			llvm_ctx.builder.CreateIsNotNull(should_free),
			bb_free,
			bb_skip);
	llvm_ctx.builder.SetInsertPoint(bb_free);
	zend_jit_zval_ptr_dtor_ex(llvm_ctx, should_free, NULL, ssa_var, info, opline->lineno, 0);
	llvm_ctx.builder.CreateBr(bb_skip);
	llvm_ctx.builder.SetInsertPoint(bb_skip);

	return 1;
}
/* }}} */

/* {{{ static void zend_jit_add_char_to_string */
static void zend_jit_add_char_to_string(zend_llvm_ctx    &llvm_ctx,
                                        Value            *result,
                                        int               result_ssa_var,
                                        uint32_t          result_info,
                                        Value            *op1,
                                        int               op1_ssa,
                                        uint32_t          op1_info,
                                        char              c,
                                        zend_op          *opline)
{
	Value *op1_val, *result_len;
	Value *op1_str = zend_jit_load_str(llvm_ctx, op1, op1_ssa, op1_info);
	Value *op1_len = zend_jit_load_str_len(llvm_ctx, op1_str);

	result_len = llvm_ctx.builder.CreateAdd(op1_len, LLVM_GET_LONG(1));
	op1_str = zend_jit_string_realloc(llvm_ctx, op1_str, result_len);
	op1_val = zend_jit_load_str_val(llvm_ctx, op1_str);

	//JIT: buf->val[length - 1] = (char) Z_LVAL_P(op2);
	llvm_ctx.builder.CreateAlignedStore(
			llvm_ctx.builder.getInt8(c),
				llvm_ctx.builder.CreateGEP(op1_val, op1_len), 1);

	//JIT: buf->val[length] = 0;
	llvm_ctx.builder.CreateAlignedStore(
			llvm_ctx.builder.getInt8(0),
				llvm_ctx.builder.CreateGEP(op1_val, result_len), 1);

	//JIT: ZVAL_NEW_STR(result, buf);
	zend_jit_save_zval_str(llvm_ctx, result, result_ssa_var, result_info, op1_str);
	zend_jit_save_zval_type_info(llvm_ctx, result, result_ssa_var, result_info, llvm_ctx.builder.getInt32(IS_STRING_EX));
}
/* }}} */

/* {{{ static void zend_jit_add_string_to_string */
static void zend_jit_add_string_to_string(zend_llvm_ctx    &llvm_ctx,
                                          Value            *result,
                                          int               result_ssa_var,
                                          uint32_t          result_info,
                                          Value            *op1,
                                          int               op1_ssa,
                                          uint32_t          op1_info,
										  zend_string      *str,
                                          zend_op          *opline)
{
	Value *op1_val, *result_len;
	Value *op1_str = zend_jit_load_str(llvm_ctx, op1, op1_ssa, op1_info);
	Value *op1_len = zend_jit_load_str_len(llvm_ctx, op1_str);
	Value *op2_val = LLVM_GET_CONST_STRING(str->val);
	Value *op2_len = LLVM_GET_LONG(str->len);

	result_len = llvm_ctx.builder.CreateAdd(op1_len, op2_len);
	op1_str = zend_jit_string_realloc(llvm_ctx, op1_str, result_len);
	op1_val = zend_jit_load_str_val(llvm_ctx, op1_str);

	//JIT: memcpy(buf->val + op1_len, Z_STRVAL_P(op2), Z_STRLEN_P(op2));
	llvm_ctx.builder.CreateMemCpy(
			llvm_ctx.builder.CreateGEP(op1_val, op1_len), op2_val, op2_len, 1);

	//JIT: buf->val[length] = 0;
	llvm_ctx.builder.CreateAlignedStore(
			llvm_ctx.builder.getInt8(0),
				llvm_ctx.builder.CreateGEP(op1_val, result_len), 1);

	//JIT: ZVAL_NEW_STR(result, buf);
	zend_jit_save_zval_str(llvm_ctx, result, result_ssa_var, result_info, op1_str);
	zend_jit_save_zval_type_info(llvm_ctx, result, result_ssa_var, result_info, llvm_ctx.builder.getInt32(IS_STRING_EX));
}
/* }}} */

/* {{{ static void zend_jit_add_var_to_string */
static void zend_jit_add_var_to_string(zend_llvm_ctx    &llvm_ctx,
                                       Value            *result,
                                       int               result_ssa_var,
                                       uint32_t          result_info,
                                       Value            *op1,
                                       int               op1_ssa,
                                       uint32_t          op1_info,
                                       Value            *str,
                                       zend_op          *opline)
{
	Value *op1_val, *result_len;
	Value *op1_str = zend_jit_load_str(llvm_ctx, op1, op1_ssa, op1_info);
	Value *op1_len = zend_jit_load_str_len(llvm_ctx, op1_str);
	Value *op2_val = zend_jit_load_str_val(llvm_ctx, str);
	Value *op2_len = zend_jit_load_str_len(llvm_ctx, str);

	result_len = llvm_ctx.builder.CreateAdd(op1_len, op2_len);
	op1_str = zend_jit_string_realloc(llvm_ctx, op1_str, result_len);
	op1_val = zend_jit_load_str_val(llvm_ctx, op1_str);

	//JIT: memcpy(buf->val + op1_len, Z_STRVAL_P(op2), Z_STRLEN_P(op2));
	llvm_ctx.builder.CreateMemCpy(
			llvm_ctx.builder.CreateGEP(op1_val, op1_len), op2_val, op2_len, 1);

	//JIT: buf->val[length] = 0;
	llvm_ctx.builder.CreateAlignedStore(
			llvm_ctx.builder.getInt8(0),
				llvm_ctx.builder.CreateGEP(op1_val, result_len), 1);

	//JIT: ZVAL_NEW_STR(result, buf);
	zend_jit_save_zval_str(llvm_ctx, result, result_ssa_var, result_info, op1_str);
	zend_jit_save_zval_type_info(llvm_ctx, result, result_ssa_var, result_info, llvm_ctx.builder.getInt32(IS_STRING_EX));
}
/* }}} */

/* {{{ static void zend_jit_make_printable_zval */
static void zend_jit_make_printable_zval(zend_llvm_ctx    &llvm_ctx,
                                         Value            *expr,
                                         Value           **expr_type,
                                         uint32_t          expr_op_type,
                                         znode_op         *expr_op,
                                         int               expr_ssa_var,
                                         uint32_t          expr_info,
                                         BasicBlock      **bb_string,
										 Value           **expr_str,
                                         BasicBlock      **bb_not_string,
                                         Value           **copy_str,
                                         zend_op          *opline)
{
	if (expr_op_type == IS_CONST) {
		zval str;
		int use_copy = zend_make_printable_zval(RT_CONSTANT(llvm_ctx.op_array, *expr_op), &str);

		if (use_copy) {
			*copy_str = llvm_ctx.builder.CreateIntToPtr(
					LLVM_GET_LONG((zend_uintptr_t)Z_STR(str)),
					PointerType::getUnqual(llvm_ctx.zend_string_type));
			if (!*bb_not_string) {
				*bb_not_string = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(*bb_not_string);
		} else {
			if (!*bb_string) {
				*bb_string = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			*expr_str = llvm_ctx.builder.CreateIntToPtr(
					LLVM_GET_LONG((zend_uintptr_t)Z_STR_P(RT_CONSTANT(llvm_ctx.op_array, *expr_op))),
					PointerType::getUnqual(llvm_ctx.zend_string_type));
			llvm_ctx.builder.CreateBr(*bb_string);
		}
	} else {
		BasicBlock *bb_follow = NULL;
		BasicBlock *bb_copy = NULL;
		PHI_DCL(val, 3);
		// JIT: if (Z_TYPE_P(expr) == IS_STRING)
		if (expr_info & MAY_BE_STRING) {
			if (!*bb_string) {
				*bb_string = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}

			if (expr_info & (MAY_BE_ANY - MAY_BE_STRING)) {
				BasicBlock *bb_str = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

				if (!*expr_type) {
					*expr_type = zend_jit_load_type(llvm_ctx, expr, expr_ssa_var, expr_info);
				}

				zend_jit_expected_br(llvm_ctx,
						llvm_ctx.builder.CreateICmpEQ(
							*expr_type,
							llvm_ctx.builder.getInt8(IS_STRING)),
						bb_str,
						bb_follow);
				llvm_ctx.builder.SetInsertPoint(bb_str);
				*expr_str = zend_jit_load_str(llvm_ctx, expr, expr_ssa_var, expr_info);
				llvm_ctx.builder.CreateBr(*bb_string);
			} else {
				*expr_str = zend_jit_load_str(llvm_ctx, expr, expr_ssa_var, expr_info);
				llvm_ctx.builder.CreateBr(*bb_string);
			}
		}

		if (expr_info & (MAY_BE_ANY - MAY_BE_STRING)) {
			bb_copy = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		}

		// JIT: case IS_LONG:
		if (expr_info & MAY_BE_LONG) {
			if (bb_follow) {
				llvm_ctx.builder.SetInsertPoint(bb_follow);
				bb_follow = NULL;
			}

			if (expr_info & (MAY_BE_ANY - (MAY_BE_LONG|MAY_BE_STRING))) {
				BasicBlock *bb_long = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

				if (!*expr_type) {
					*expr_type = zend_jit_load_type(llvm_ctx, expr, expr_ssa_var, expr_info);
				}
				zend_jit_expected_br(llvm_ctx,
						llvm_ctx.builder.CreateICmpEQ(
							*expr_type,
							llvm_ctx.builder.getInt8(IS_LONG)),
						bb_long,
						bb_follow);
				llvm_ctx.builder.SetInsertPoint(bb_long);
			}

			*copy_str = zend_jit_long_to_str(llvm_ctx,
					zend_jit_load_lval_c(llvm_ctx, expr, expr_op_type, expr_op, expr_ssa_var, expr_info));

			PHI_ADD(val, *copy_str);

			llvm_ctx.builder.CreateBr(bb_copy);
		}

		// JIT: case IS_DOUBLE:
		if (expr_info & MAY_BE_DOUBLE) {
			if (bb_follow) {
				llvm_ctx.builder.SetInsertPoint(bb_follow);
				bb_follow = NULL;
			}

			if (expr_info & (MAY_BE_ANY - (MAY_BE_DOUBLE|MAY_BE_LONG|MAY_BE_STRING))) {
				BasicBlock *bb_double = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

				if (!*expr_type) {
					*expr_type = zend_jit_load_type(llvm_ctx, expr, expr_ssa_var, expr_info);
				}

				zend_jit_expected_br(llvm_ctx,
						llvm_ctx.builder.CreateICmpEQ(
							*expr_type,
							llvm_ctx.builder.getInt8(IS_DOUBLE)),
						bb_double,
						bb_follow);
				llvm_ctx.builder.SetInsertPoint(bb_double);
			}

			Value *precision = llvm_ctx.builder.CreateAlignedLoad(llvm_ctx._EG_precision, 4);

			*copy_str = zend_jit_strpprintf(llvm_ctx,
					LLVM_GET_LONG(0),
					LLVM_GET_CONST_STRING("%.*G"),
					llvm_ctx.builder.CreateTruncOrBitCast(
						precision,
						Type::getInt32Ty(llvm_ctx.context)),
					zend_jit_load_dval(llvm_ctx, expr, expr_ssa_var, expr_info));

			PHI_ADD(val, *copy_str);

			llvm_ctx.builder.CreateBr(bb_copy);
		}

		if (bb_follow || (expr_info & (MAY_BE_ANY-(MAY_BE_DOUBLE|MAY_BE_LONG|MAY_BE_STRING)))) {
			// slow path
			if (bb_follow) {
				llvm_ctx.builder.SetInsertPoint(bb_follow);
				bb_follow = NULL;
			}
			if (expr_info & (MAY_BE_OBJECT|MAY_BE_ARRAY)) {
				if (!llvm_ctx.valid_opline) {
					zend_jit_store_opline(llvm_ctx, opline, false);
				}
			}

			if (expr_info & MAY_BE_IN_REG) {
				expr = zend_jit_reload_from_reg(llvm_ctx, expr_ssa_var, expr_info);
			}

			*copy_str = zend_jit_zval_get_string_func(llvm_ctx, expr);

			PHI_ADD(val, *copy_str);

			llvm_ctx.builder.CreateBr(bb_copy);
		}

		if (bb_copy) {
			llvm_ctx.builder.SetInsertPoint(bb_copy);

			PHI_SET(val, *copy_str, PointerType::getUnqual(llvm_ctx.zend_string_type));

			*bb_not_string = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			llvm_ctx.builder.CreateBr(*bb_not_string);
		}
	}
}
/* }}} */

/* {{{ static zend_class_entry* zend_jit_load_operand_scope */
static zend_class_entry* zend_jit_load_operand_scope(zend_llvm_ctx    &llvm_ctx,
	 												 int               ssa_var,
													 uint32_t          op_type,
                                                     zend_jit_context *ctx,
													 zend_op_array    *op_array)
		
{
	zend_class_entry *scope = NULL;

	if (op_type == IS_UNUSED && op_array->scope) {
		zend_string *lcname = zend_string_init(op_array->scope->name->val, op_array->scope->name->len, 0);
		zend_str_tolower(lcname->val, lcname->len);
		if (zend_hash_exists(EG(class_table), lcname)
			   	|| (ctx->main_persistent_script->class_table.nNumOfElements 
					&& zend_hash_exists(&ctx->main_persistent_script->class_table, lcname))) {
			scope = op_array->scope;
		}
		zend_string_release(lcname);
	} else {
		zend_jit_ssa_var_info *op_info = (ssa_var >= 0)? &JIT_DATA(llvm_ctx.op_array)->ssa_var_info[ssa_var] : NULL;
		if (op_info && op_info->ce && !op_info->is_instanceof) {
			scope = op_info->ce;
		}
	}

	return scope;
}
/* }}} */

/* {{{ static int zend_jit_hash_find */
static Value* zend_jit_hash_find(zend_llvm_ctx    &llvm_ctx,
                                 Value            *ht,
                                 Value            *key)
{
	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)zend_hash_find,
			ZEND_JIT_SYM("zend_hash_find"),
			ZEND_JIT_HELPER_FAST_CALL |
			ZEND_JIT_HELPER_ARG1_NOALIAS | ZEND_JIT_HELPER_ARG1_NOCAPTURE |
			ZEND_JIT_HELPER_ARG2_NOALIAS | ZEND_JIT_HELPER_ARG2_NOCAPTURE,
			llvm_ctx.zval_ptr_type,
			PointerType::getUnqual(llvm_ctx.HashTable_type),
			PointerType::getUnqual(llvm_ctx.zend_string_type),
			NULL,
			NULL,
			NULL);

	CallInst *call = llvm_ctx.builder.CreateCall2(
		_helper, ht, key);
	call->setCallingConv(CallingConv::X86_FastCall);
	return call;
}
/* }}} */

/* {{{ static int zend_jit_hash_index_find */
static Value* zend_jit_hash_index_find(zend_llvm_ctx    &llvm_ctx,
                                       Value            *ht,
                                       Value            *key)
{
	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)zend_hash_index_find,
			ZEND_JIT_SYM("zend_hash_index_find"),
			ZEND_JIT_HELPER_FAST_CALL |
			ZEND_JIT_HELPER_ARG1_NOALIAS | ZEND_JIT_HELPER_ARG1_NOCAPTURE,
			llvm_ctx.zval_ptr_type,
			PointerType::getUnqual(llvm_ctx.HashTable_type),
			LLVM_GET_LONG_TY(llvm_ctx.context),
			NULL,
			NULL,
			NULL);

	CallInst *call = llvm_ctx.builder.CreateCall2(_helper, ht, key);
	call->setCallingConv(CallingConv::X86_FastCall);
	return call;
}
/* }}} */

/* {{{ static int zend_jit_hash_update */
static Value* zend_jit_hash_update(zend_llvm_ctx    &llvm_ctx,
                                   Value            *ht,
                                   Value            *key,
                                   Value            *val,
								   zend_op          *opline)
{
	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)_zend_hash_update,
			ZEND_JIT_SYM("_zend_hash_update"),
			ZEND_JIT_HELPER_FAST_CALL |
			ZEND_JIT_HELPER_ARG1_NOALIAS | ZEND_JIT_HELPER_ARG1_NOCAPTURE |
			ZEND_JIT_HELPER_ARG2_NOALIAS | ZEND_JIT_HELPER_ARG2_NOCAPTURE |
			ZEND_JIT_HELPER_ARG3_NOALIAS | ZEND_JIT_HELPER_ARG3_NOCAPTURE,
			llvm_ctx.zval_ptr_type,
			PointerType::getUnqual(llvm_ctx.HashTable_type),
			PointerType::getUnqual(llvm_ctx.zend_string_type),
			llvm_ctx.zval_ptr_type,
#if ZEND_DEBUG
			PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context)),
			Type::getInt32Ty(llvm_ctx.context)
#else
			NULL,
			NULL
#endif
			);

#if ZEND_DEBUG
	CallInst *call = llvm_ctx.builder.CreateCall5(
			_helper, 
			ht, 
			key, 
			val, 
			zend_jit_function_name(llvm_ctx),
			llvm_ctx.builder.getInt32(opline->lineno));
#else
	CallInst *call = llvm_ctx.builder.CreateCall3(
			_helper, 
			ht, 
			key, 
			val);
#endif
	call->setCallingConv(CallingConv::X86_FastCall);
	return call;
}
/* }}} */

/* {{{ static int zend_jit_hash_index_update */
static Value* zend_jit_hash_index_update(zend_llvm_ctx    &llvm_ctx,
                                         Value            *ht,
                                         Value            *key,
                                         Value            *val,
                                         zend_op          *opline)
{
	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)_zend_hash_index_update,
			ZEND_JIT_SYM("_zend_hash_index_update"),
			ZEND_JIT_HELPER_FAST_CALL |
			ZEND_JIT_HELPER_ARG1_NOALIAS | ZEND_JIT_HELPER_ARG1_NOCAPTURE |
			ZEND_JIT_HELPER_ARG3_NOALIAS | ZEND_JIT_HELPER_ARG3_NOCAPTURE,
			llvm_ctx.zval_ptr_type,
			PointerType::getUnqual(llvm_ctx.HashTable_type),
			LLVM_GET_LONG_TY(llvm_ctx.context),
			llvm_ctx.zval_ptr_type,
#if ZEND_DEBUG
			PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context)),
			Type::getInt32Ty(llvm_ctx.context)
#else
			NULL,
			NULL
#endif
			);

#if ZEND_DEBUG
	CallInst *call = llvm_ctx.builder.CreateCall5(
			_helper, 
			ht, 
			key, 
			val, 
			zend_jit_function_name(llvm_ctx),
			llvm_ctx.builder.getInt32(opline->lineno));
#else
	CallInst *call = llvm_ctx.builder.CreateCall3(
			_helper, 
			ht, 
			key, 
			val);
#endif
	call->setCallingConv(CallingConv::X86_FastCall);
	return call;
}
/* }}} */

/* {{{ static int zend_jit_hash_next_index_insert */
static Value* zend_jit_hash_next_index_insert(zend_llvm_ctx    &llvm_ctx,
                                              Value            *ht,
                                              Value            *val,
                                              zend_op          *opline)
{
	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)_zend_hash_next_index_insert,
			ZEND_JIT_SYM("_zend_hash_next_index_insert"),
			ZEND_JIT_HELPER_FAST_CALL |
			ZEND_JIT_HELPER_ARG1_NOALIAS | ZEND_JIT_HELPER_ARG1_NOCAPTURE |
			ZEND_JIT_HELPER_ARG2_NOALIAS | ZEND_JIT_HELPER_ARG2_NOCAPTURE,
			llvm_ctx.zval_ptr_type,
			PointerType::getUnqual(llvm_ctx.HashTable_type),
			llvm_ctx.zval_ptr_type,
#if ZEND_DEBUG
			PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context)),
			Type::getInt32Ty(llvm_ctx.context),
#else
			NULL,
			NULL,
#endif
			NULL
			);

#if ZEND_DEBUG
	CallInst *call = llvm_ctx.builder.CreateCall4(
			_helper, 
			ht, 
			val, 
			zend_jit_function_name(llvm_ctx),
			llvm_ctx.builder.getInt32(opline->lineno));
#else
	CallInst *call = llvm_ctx.builder.CreateCall2(
			_helper, 
			ht, 
			val);
#endif
	call->setCallingConv(CallingConv::X86_FastCall);
	return call;
}
/* }}} */

/* {{{ zend_jit_emalloc */
static Value* zend_jit_emalloc(zend_llvm_ctx    &llvm_ctx,
                               Value            *size,
                               uint32_t          lineno)
{
	CallInst *call = NULL;

#if ZEND_DEBUG
	Function *_helper = zend_jit_get_helper(
		llvm_ctx,
		(void*)_emalloc,
		ZEND_JIT_SYM("_emalloc"),
		ZEND_JIT_HELPER_RET_NOALIAS | ZEND_JIT_HELPER_FAST_CALL,
		PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context)),
		LLVM_GET_LONG_TY(llvm_ctx.context),
		PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context)),
		Type::getInt32Ty(llvm_ctx.context),
		PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context)),
		Type::getInt32Ty(llvm_ctx.context));
	call = llvm_ctx.builder.CreateCall5(_helper, size,
		zend_jit_function_name(llvm_ctx),
		llvm_ctx.builder.getInt32(lineno),
		llvm_ctx.builder.CreateIntToPtr(
			LLVM_GET_LONG(0),
			PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context))),
		llvm_ctx.builder.getInt32(0));
#else
	if (!llvm_ctx.mm_heap) {
		Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)_emalloc,
			ZEND_JIT_SYM("_emalloc"),
			ZEND_JIT_HELPER_RET_NOALIAS | ZEND_JIT_HELPER_FAST_CALL,
			PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context)),
			LLVM_GET_LONG_TY(llvm_ctx.context),
			NULL,
			NULL,
			NULL,
			NULL);
		call = llvm_ctx.builder.CreateCall(_helper, size);
	} else {
		Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			llvm_ctx.mm_alloc,
			ZEND_JIT_SYM("_zend_mm_alloc"),
			ZEND_JIT_HELPER_RET_NOALIAS | ZEND_JIT_HELPER_FAST_CALL,
			PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context)),
			PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context)),
			LLVM_GET_LONG_TY(llvm_ctx.context),
			NULL,
			NULL,
			NULL);
		call = llvm_ctx.builder.CreateCall2(
			_helper,
			llvm_ctx.builder.CreateIntToPtr(
				LLVM_GET_LONG((zend_uintptr_t)llvm_ctx.mm_heap),
				PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))),
			size);
	}
#endif
	call->setCallingConv(CallingConv::X86_FastCall);
	return call;
}
/* }}} */

/* {{{ zend_jit_efree */
static Value* zend_jit_efree(zend_llvm_ctx    &llvm_ctx,
                             Value            *ptr,
                             uint32_t          lineno)
{
	CallInst *call = NULL;

#if ZEND_DEBUG
	Function *_helper = zend_jit_get_helper(
		llvm_ctx,
		(void*)_efree,
		ZEND_JIT_SYM("_free"),
		ZEND_JIT_HELPER_FAST_CALL | ZEND_JIT_HELPER_ARG1_NOALIAS | ZEND_JIT_HELPER_ARG1_NOCAPTURE,
		Type::getVoidTy(llvm_ctx.context),
		PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context)),
		PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context)),
		Type::getInt32Ty(llvm_ctx.context),
		PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context)),
		Type::getInt32Ty(llvm_ctx.context));
	call = llvm_ctx.builder.CreateCall5(_helper,
		llvm_ctx.builder.CreateBitCast(
			ptr,
			PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))),
		zend_jit_function_name(llvm_ctx),
		llvm_ctx.builder.getInt32(lineno),
		llvm_ctx.builder.CreateIntToPtr(
			LLVM_GET_LONG(0),
			PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context))),
		llvm_ctx.builder.getInt32(0));
#else
	if (!llvm_ctx.mm_heap) {
		Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)_efree,
			ZEND_JIT_SYM("_efree"),
			ZEND_JIT_HELPER_FAST_CALL | ZEND_JIT_HELPER_ARG1_NOALIAS | ZEND_JIT_HELPER_ARG1_NOCAPTURE,
			Type::getVoidTy(llvm_ctx.context),
			PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context)),
			NULL,
			NULL,
			NULL,
			NULL);
		call = llvm_ctx.builder.CreateCall(_helper, 
			llvm_ctx.builder.CreateBitCast(
				ptr,
				PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))));
	} else {
		Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			llvm_ctx.mm_free,
			ZEND_JIT_SYM("_zend_mm_free"),
			ZEND_JIT_HELPER_FAST_CALL | ZEND_JIT_HELPER_ARG1_NOALIAS | ZEND_JIT_HELPER_ARG1_NOCAPTURE,
			Type::getVoidTy(llvm_ctx.context),
			PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context)),
			PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context)),
			NULL,
			NULL,
			NULL);
		call = llvm_ctx.builder.CreateCall2(
			_helper,
			llvm_ctx.builder.CreateIntToPtr(
				LLVM_GET_LONG((zend_uintptr_t)llvm_ctx.mm_heap),
				PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))),
			llvm_ctx.builder.CreateBitCast(
				ptr,
				PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))));
	}
#endif
	call->setCallingConv(CallingConv::X86_FastCall);
	return call;
}
/* }}} */

/* {{{ static int zend_jit_cache_slot_addr */
static Value *zend_jit_cache_slot_addr(zend_llvm_ctx    &llvm_ctx,
                                       uint32_t          cache_slot,
                                       Type             *type)
{
	return zend_jit_GEP(
			llvm_ctx,
			llvm_ctx.builder.CreateAlignedLoad(
				zend_jit_GEP(
					llvm_ctx,
					llvm_ctx._execute_data,
					offsetof(zend_execute_data, run_time_cache),
					PointerType::getUnqual(PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context)))), 4),
			cache_slot,
			PointerType::getUnqual(type));
}
/* }}} */

/* {{{ static Value* zend_jit_fetch_dimension_address_inner */
static Value* zend_jit_fetch_dimension_address_inner(zend_llvm_ctx   &llvm_ctx,
                                                     Value           *ht,
                                                     uint32_t        array_info,
                                                     Value           *dim,
                                                     Value           *dim_type,
                                                     int              dim_ssa,
                                                     uint32_t         dim_info,
                                                     uint32_t         dim_op_type,
                                                     znode_op        *dim_op,
													 Value          **new_element,
													 BasicBlock     **bb_new_element,
                                                     BasicBlock     **bb_uninitialized,
													 BasicBlock     **bb_error,
                                                     uint32_t         fetch_type,
                                                     zend_op         *opline)
{
	Value *retval = NULL;
	BasicBlock *bb_follow = NULL;
	BasicBlock *bb_fetch_string_dim = NULL;
	BasicBlock *bb_fetch_number_dim = NULL;
	BasicBlock *bb_finish = NULL;
	BasicBlock *bb_add_new = NULL;
	PHI_DCL(offset, 3);
	PHI_DCL(index, 7);
	PHI_DCL(ret, 5);
	PHI_DCL(new_elem, 2);

	// JIT: switch(dim->type)
	if (dim_op_type == IS_CONST) {
		zend_ulong num_index;
		switch (Z_TYPE_P(RT_CONSTANT(llvm_ctx.op_array, *dim_op))) {
			case IS_DOUBLE:
				num_index = zend_dval_to_lval(Z_DVAL_P(RT_CONSTANT(llvm_ctx.op_array, *dim_op)));
				goto numeric_dim;
				break;
			case IS_TRUE:
				num_index = 1;
				goto numeric_dim;
			case IS_FALSE:
				num_index = 0;
				goto numeric_dim;
			case IS_LONG:
				num_index = Z_LVAL_P(RT_CONSTANT(llvm_ctx.op_array, *dim_op));
numeric_dim:
				PHI_ADD(index, LLVM_GET_LONG(num_index));
				break;
			case IS_NULL:
				PHI_ADD(offset, llvm_ctx._CG_empty_string);
				break;
			case IS_STRING:
				if (ZEND_HANDLE_NUMERIC(Z_STR_P(RT_CONSTANT(llvm_ctx.op_array, *dim_op)), num_index)) {
					goto numeric_dim;
				} else {
					PHI_ADD(offset,
							llvm_ctx.builder.CreateIntToPtr(
								LLVM_GET_LONG((zend_uintptr_t)Z_STR_P(RT_CONSTANT(llvm_ctx.op_array, *dim_op))),
								PointerType::getUnqual(llvm_ctx.zend_string_type)));
				}
				break;
			default:
				ASSERT_NOT_REACHED();
		}
	} else {
		// JIT: case IS_LONG
		if ((dim_info & MAY_BE_LONG)) {
			if (!bb_fetch_number_dim) {
				bb_fetch_number_dim = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}

			if ((dim_info & (MAY_BE_ANY - MAY_BE_LONG))) {
				BasicBlock *bb_long = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				if (!dim_type) {
					dim_type = zend_jit_load_type(llvm_ctx, dim, dim_ssa, dim_info);
				}
				zend_jit_expected_br(llvm_ctx,
					llvm_ctx.builder.CreateICmpEQ(
						dim_type,
						llvm_ctx.builder.getInt8(IS_LONG)),
					bb_long,
					bb_follow);
				llvm_ctx.builder.SetInsertPoint(bb_long);
			}

			PHI_ADD(index, zend_jit_load_lval_c(llvm_ctx, dim, dim_op_type, dim_op, dim_ssa, dim_info));
			llvm_ctx.builder.CreateBr(bb_fetch_number_dim);
		}

		// JIT: case IS_STRING:
		if ((dim_info & MAY_BE_STRING)) {
			Value *str, *hval;

			if (bb_follow) {
				llvm_ctx.builder.SetInsertPoint(bb_follow);
				bb_follow = NULL;
			}
			if (!bb_fetch_string_dim) {
				bb_fetch_string_dim = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			if (!bb_fetch_number_dim) {
				bb_fetch_number_dim = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}

			if ((dim_info & (MAY_BE_ANY - (MAY_BE_LONG|MAY_BE_STRING)))) {
				BasicBlock *bb_str = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				if (!dim_type) {
					dim_type = zend_jit_load_type(llvm_ctx, dim, dim_ssa, dim_info);
				}
				zend_jit_expected_br(llvm_ctx,
					llvm_ctx.builder.CreateICmpEQ(
						dim_type,
						llvm_ctx.builder.getInt8(IS_STRING)),
					bb_str,
					bb_follow);
				llvm_ctx.builder.SetInsertPoint(bb_str);
			}

			str = zend_jit_load_str(llvm_ctx, dim, dim_ssa, dim_info);

			BasicBlock *bb_handle_numeric = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			BasicBlock *bb_handle_string = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

			hval = llvm_ctx.builder.CreateBitCast(
						zend_jit_get_stack_slot(llvm_ctx, 0),
						PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))
					);
			zend_jit_handle_numeric(llvm_ctx,
				str, 
				hval,
				bb_handle_numeric,
				bb_handle_string,
				opline);

			llvm_ctx.builder.SetInsertPoint(bb_handle_numeric);

			PHI_ADD(index, llvm_ctx.builder.CreateAlignedLoad(hval, 4));
			llvm_ctx.builder.CreateBr(bb_fetch_number_dim);

			llvm_ctx.builder.SetInsertPoint(bb_handle_string);

			PHI_ADD(offset, str);

			llvm_ctx.builder.CreateBr(bb_fetch_string_dim);
		}

		// JIT: case IS_NULL:
		if ((dim_info & MAY_BE_NULL)) {
			if (bb_follow) {
				llvm_ctx.builder.SetInsertPoint(bb_follow);
				bb_follow = NULL;
			}
			if (!bb_fetch_string_dim) {
				bb_fetch_string_dim = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			if ((dim_info & (MAY_BE_ANY - (MAY_BE_LONG|MAY_BE_STRING|MAY_BE_NULL)))) {
				BasicBlock *bb_null = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				if (!dim_type) {
					dim_type = zend_jit_load_type(llvm_ctx, dim, dim_ssa, dim_info);
				}
				zend_jit_expected_br(llvm_ctx,
					llvm_ctx.builder.CreateICmpEQ(
						dim_type,
						llvm_ctx.builder.getInt8(IS_NULL)),
					bb_null,
					bb_follow);
				llvm_ctx.builder.SetInsertPoint(bb_null);
			}

			PHI_ADD(offset, llvm_ctx._CG_empty_string);
			llvm_ctx.builder.CreateBr(bb_fetch_string_dim);
		}

		// JIT: case IS_DOUBLE
		if ((dim_info & (MAY_BE_DOUBLE))) {
			if (bb_follow) {
				llvm_ctx.builder.SetInsertPoint(bb_follow);
				bb_follow = NULL;
			}

			if (!bb_fetch_number_dim) {
				bb_fetch_number_dim = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}

			if ((dim_info & (MAY_BE_ANY - (MAY_BE_LONG|MAY_BE_STRING|MAY_BE_NULL|MAY_BE_DOUBLE)))) {
				BasicBlock *bb_double = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				if (!dim_type) {
					dim_type = zend_jit_load_type(llvm_ctx, dim, dim_ssa, dim_info);
				}
				zend_jit_expected_br(llvm_ctx,
					llvm_ctx.builder.CreateICmpEQ(
						dim_type,
						llvm_ctx.builder.getInt8(IS_DOUBLE)),
					bb_double,
					bb_follow);
				llvm_ctx.builder.SetInsertPoint(bb_double);
			}

			PHI_ADD(index, zend_jit_dval_to_lval(llvm_ctx, zend_jit_load_dval(llvm_ctx, dim, dim_ssa, dim_info)));
			llvm_ctx.builder.CreateBr(bb_fetch_number_dim);
		}

		// JIT: case IS_TRUE
		if ((dim_info & MAY_BE_TRUE)) {
			if (bb_follow) {
				llvm_ctx.builder.SetInsertPoint(bb_follow);
				bb_follow = NULL;
			}
			if (!bb_fetch_number_dim) {
				bb_fetch_number_dim = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}

			if ((dim_info & (MAY_BE_ANY - (MAY_BE_LONG|MAY_BE_STRING|MAY_BE_NULL|MAY_BE_DOUBLE|MAY_BE_TRUE)))) {
				BasicBlock *bb_true = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				if (!dim_type) {
					dim_type = zend_jit_load_type(llvm_ctx, dim, dim_ssa, dim_info);
				}
				zend_jit_expected_br(llvm_ctx,
					llvm_ctx.builder.CreateICmpEQ(
						dim_type,
						llvm_ctx.builder.getInt8(IS_TRUE)),
					bb_true,
					bb_follow);
				llvm_ctx.builder.SetInsertPoint(bb_true);
			}

			PHI_ADD(index, LLVM_GET_LONG(1));

			llvm_ctx.builder.CreateBr(bb_fetch_number_dim);
		}
		
		// JIT: case IS_FALSE
		if ((dim_info & MAY_BE_FALSE)) {
			if (bb_follow) {
				llvm_ctx.builder.SetInsertPoint(bb_follow);
				bb_follow = NULL;
			}
			if (!bb_fetch_number_dim) {
				bb_fetch_number_dim = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}

			if ((dim_info & (MAY_BE_ANY - (MAY_BE_LONG|MAY_BE_STRING|MAY_BE_NULL|MAY_BE_DOUBLE|MAY_BE_TRUE|MAY_BE_FALSE)))) {
				BasicBlock *bb_false = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				if (!dim_type) {
					dim_type = zend_jit_load_type(llvm_ctx, dim, dim_ssa, dim_info);
				}
				zend_jit_expected_br(llvm_ctx,
					llvm_ctx.builder.CreateICmpEQ(
						dim_type,
						llvm_ctx.builder.getInt8(IS_FALSE)),
					bb_false,
					bb_follow);
				llvm_ctx.builder.SetInsertPoint(bb_false);
			}

			PHI_ADD(index, LLVM_GET_LONG(0));

			llvm_ctx.builder.CreateBr(bb_fetch_number_dim);
		}
	
		// JIT: case IS_RESOURCE
		if ((dim_info & (MAY_BE_RESOURCE))) {
			Value *res, *handle;

			if (bb_follow) {
				llvm_ctx.builder.SetInsertPoint(bb_follow);
				bb_follow = NULL;
			}
			if (!bb_fetch_number_dim) {
				bb_fetch_number_dim = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}

			if ((dim_info & (MAY_BE_ANY - (MAY_BE_LONG|MAY_BE_STRING|MAY_BE_NULL|MAY_BE_DOUBLE|MAY_BE_TRUE|MAY_BE_FALSE|MAY_BE_RESOURCE)))) {
				BasicBlock *bb_resource = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				if (!dim_type) {
					dim_type = zend_jit_load_type(llvm_ctx, dim, dim_ssa, dim_info);
				}
				zend_jit_expected_br(llvm_ctx,
					llvm_ctx.builder.CreateICmpEQ(
						dim_type,
						llvm_ctx.builder.getInt8(IS_RESOURCE)),
					bb_resource,
					bb_follow);
				llvm_ctx.builder.SetInsertPoint(bb_resource);
			}

			res = zend_jit_load_res(llvm_ctx, dim, dim_ssa, dim_info);
			handle = zend_jit_load_res_handle(llvm_ctx, res);
			zend_jit_error(llvm_ctx, 
				opline,
				E_NOTICE,
				"Resource ID#%ld used as offset, casting to integer (%ld)",
				handle, handle);

			PHI_ADD(index, handle);

			llvm_ctx.builder.CreateBr(bb_fetch_number_dim);
		}

		if (bb_follow || (dim_info & (MAY_BE_ANY - (MAY_BE_NULL|MAY_BE_STRING|MAY_BE_DOUBLE|MAY_BE_RESOURCE|MAY_BE_TRUE|MAY_BE_FALSE|MAY_BE_LONG)))) {
			// JIT: default:
			if (bb_follow) {
				llvm_ctx.builder.SetInsertPoint(bb_follow);
			}

			zend_jit_error(
				llvm_ctx,
				opline,
				E_WARNING, 
				"%s", 
				LLVM_GET_CONST_STRING("Illegal offset type"));

			if (fetch_type == BP_VAR_W || fetch_type == BP_VAR_RW) {
				if (!*bb_error) {
					*bb_error = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				}
				llvm_ctx.builder.CreateBr(*bb_error);
			} else {
				if (!*bb_uninitialized) {
					*bb_uninitialized = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				}
				llvm_ctx.builder.CreateBr(*bb_uninitialized);
			}
		}
	}

	// JIT: lable fetch_string_dim:
	if (PHI_COUNT(offset)) {
		Value *str;

		if (bb_fetch_string_dim) {
			llvm_ctx.builder.SetInsertPoint(bb_fetch_string_dim);
		}

		PHI_SET(offset, str, PointerType::getUnqual(llvm_ctx.zend_string_type));

		if (array_info &&
				(((array_info & MAY_BE_ARRAY_KEY_ANY) == MAY_BE_ARRAY_KEY_ANY) ||
				 !(array_info & MAY_BE_ARRAY_KEY_LONG))) {
			BasicBlock *bb_found = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			BasicBlock *bb_not_found = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			BasicBlock *bb_not_found_indirect = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			BasicBlock *bb_indirect = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			BasicBlock *bb_not_ind = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			BasicBlock *bb_cont = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			Value *zv = zend_jit_hash_find(llvm_ctx, ht, str);

			zend_jit_unexpected_br(llvm_ctx,
					llvm_ctx.builder.CreateIsNull(zv),
					bb_not_found,
					bb_found);

			llvm_ctx.builder.SetInsertPoint(bb_found);

			Value *zv_type = zend_jit_load_type(llvm_ctx, zv, -1, MAY_BE_ANY);

			//JIT: if (UNEXPECTED(Z_TYPE_P(retval) == IS_INDIRECT))
			zend_jit_unexpected_br(llvm_ctx,
					llvm_ctx.builder.CreateICmpEQ(
						zv_type,
						llvm_ctx.builder.getInt8(IS_INDIRECT)),
					bb_indirect,
					bb_not_ind);
			llvm_ctx.builder.SetInsertPoint(bb_not_ind);

			PHI_ADD(ret, zv);
			if (!bb_finish) {
				bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(bb_finish);

			llvm_ctx.builder.SetInsertPoint(bb_indirect);
			zv = zend_jit_load_ind(llvm_ctx, zv);
			zv_type = zend_jit_load_type(llvm_ctx, zv, -1, MAY_BE_ANY);

			zend_jit_unexpected_br(llvm_ctx,
					llvm_ctx.builder.CreateICmpEQ(
						zv_type,
						llvm_ctx.builder.getInt8(IS_UNDEF)),
					bb_not_found_indirect,
					bb_cont);

			llvm_ctx.builder.SetInsertPoint(bb_cont);

			PHI_ADD(ret, zv);
			if (!bb_finish) {
				bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(bb_finish);

			llvm_ctx.builder.SetInsertPoint(bb_not_found_indirect);
			switch (fetch_type) {
				case BP_VAR_R:
					zend_jit_error(llvm_ctx, opline, E_NOTICE,"Undefined index: %s", zend_jit_load_str_val(llvm_ctx, str));
					/* break missing intentionally */
				case BP_VAR_UNSET:
				case BP_VAR_IS:
					if (!*bb_uninitialized) {
						*bb_uninitialized = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					}
					llvm_ctx.builder.CreateBr(*bb_uninitialized);
					break;
				case BP_VAR_RW:
					zend_jit_error(llvm_ctx, opline, E_NOTICE,"Undefined index: %s", zend_jit_load_str_val(llvm_ctx, str));
					/* break missing intentionally */
				case BP_VAR_W:
					zend_jit_save_zval_type_info(llvm_ctx, zv, -1, MAY_BE_ANY, llvm_ctx.builder.getInt32(IS_NULL));
					PHI_ADD(ret, zv);
					if (!bb_finish) {
						bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					}
					llvm_ctx.builder.CreateBr(bb_finish);
					break;
			}

			llvm_ctx.builder.SetInsertPoint(bb_not_found);
		}

		switch (fetch_type) {
			case BP_VAR_R:
				zend_jit_error(llvm_ctx, opline, E_NOTICE, "Undefined index: %s", zend_jit_load_str_val(llvm_ctx, str));
			case BP_VAR_UNSET:
			case BP_VAR_IS:
				{
					if (!*bb_uninitialized) {
						*bb_uninitialized = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					}
					llvm_ctx.builder.CreateBr(*bb_uninitialized);
				}
				break;
			case BP_VAR_RW:
				zend_jit_error(llvm_ctx, opline, E_NOTICE,"Undefined index: %s", zend_jit_load_str_val(llvm_ctx, str));
			case BP_VAR_W: {
					Value *zv = zend_jit_hash_update(
						llvm_ctx,
						ht,
						str,
						llvm_ctx._EG_uninitialized_zval,
						opline);
					PHI_ADD(new_elem, zv);
					if (!bb_add_new) {
						bb_add_new = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					}
					llvm_ctx.builder.CreateBr(bb_add_new);
				}
				break;
		}
	}
	
	// JIT: lable num_index:
	if (PHI_COUNT(index)) {
		Value *num_idx, *zv;

		if (bb_fetch_number_dim) {
			llvm_ctx.builder.SetInsertPoint(bb_fetch_number_dim);
		}

		PHI_SET(index, num_idx, LLVM_GET_LONG_TY(llvm_ctx.context));

		if (array_info &&
				((array_info & MAY_BE_ARRAY_KEY_ANY) == MAY_BE_ARRAY_KEY_ANY)
				|| !(array_info & MAY_BE_ARRAY_KEY_STRING)) {
			BasicBlock *bb_found = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			BasicBlock *bb_not_found = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			Value *zv = zend_jit_hash_index_find(llvm_ctx, ht, num_idx);

			zend_jit_unexpected_br(llvm_ctx,
					llvm_ctx.builder.CreateIsNull(zv),
					bb_not_found,
					bb_found);
			llvm_ctx.builder.SetInsertPoint(bb_found);
			PHI_ADD(ret, zv);
			if (!bb_finish) {
				bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(bb_finish);

			llvm_ctx.builder.SetInsertPoint(bb_not_found);
		}

		switch (fetch_type) {
			case BP_VAR_R:
				zend_jit_error(llvm_ctx, opline, E_NOTICE, "Undefined offset: " ZEND_LONG_FMT, num_idx);
			case BP_VAR_UNSET:
			case BP_VAR_IS:
				{
					if (!*bb_uninitialized) {
						*bb_uninitialized = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					}
					llvm_ctx.builder.CreateBr(*bb_uninitialized);
				}
				break;
			case BP_VAR_RW:
				zend_jit_error(llvm_ctx, opline, E_NOTICE, "Undefined offset: " ZEND_LONG_FMT, num_idx);
			case BP_VAR_W: 
				{
					Value *zv = zend_jit_hash_index_update(
						llvm_ctx,
						ht,
						num_idx,
						llvm_ctx._EG_uninitialized_zval,
						opline);
					PHI_ADD(new_elem, zv);
					if (!bb_add_new) {
						bb_add_new = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					}
					llvm_ctx.builder.CreateBr(bb_add_new);
				}
				break;
		}
	}

	if (bb_add_new) {
		llvm_ctx.builder.SetInsertPoint(bb_add_new);
		PHI_SET(new_elem, *new_element, llvm_ctx.zval_ptr_type);
		if (!*bb_new_element) {
			*bb_new_element = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		}
		llvm_ctx.builder.CreateBr(*bb_new_element);
	}

	if (bb_finish) {
		llvm_ctx.builder.SetInsertPoint(bb_finish);
		PHI_SET(ret, retval, llvm_ctx.zval_ptr_type);
	}

	return retval;
}
/* }}} */

/* {{{ static Value* zend_jit_str_offset_index */
static Value* zend_jit_str_offset_index(zend_llvm_ctx &llvm_ctx,
                                        Value         *container,
                                        int            container_ssa,
                                        uint32_t       container_info,
                                        Value         *dim,
                                        int            dim_ssa,
                                        uint32_t       dim_info,
                                        zend_uchar     dim_op_type,
                                        znode_op      *dim_op,
                                        uint32_t       fetch_type,
                                        zend_op       *opline)
{
	Value *str_index;
	Value *dim_type = NULL;
	PHI_DCL(index, 3);

	if (dim_op_type == IS_CONST) {
		zval tmp;
		int is_long;
		int convert = 1;
		zend_long lval = 0;

		switch (Z_TYPE_P(RT_CONSTANT(llvm_ctx.op_array, *dim_op))) {
			case IS_STRING:
				if ((is_long = is_long_numeric_string(Z_STRVAL_P(RT_CONSTANT(llvm_ctx.op_array, *dim_op)), Z_STRLEN_P(RT_CONSTANT(llvm_ctx.op_array, *dim_op)))) == 1) {
					break;
				} else if (is_long == -1 && fetch_type != BP_VAR_IS) {
					zend_jit_error(
							llvm_ctx,
							opline,
							E_NOTICE,
							"%s",
							LLVM_GET_CONST_STRING("A non well formed numeric value encountered"));
				} else if (fetch_type != BP_VAR_IS) {
					zend_jit_error(llvm_ctx, opline, E_WARNING, "Illegal string offset '%s'",
						   LLVM_GET_CONST_STRING(Z_STRVAL_P(RT_CONSTANT(llvm_ctx.op_array, *dim_op))));
				} else {
					convert = 0;
					lval = -1;
				}
				break;
			case IS_DOUBLE:
			case IS_NULL:
			case IS_TRUE:
			case IS_FALSE:
				if (fetch_type != BP_VAR_IS) {
					zend_jit_error(
						llvm_ctx,
						opline,
						E_NOTICE,
						"%s",
						LLVM_GET_CONST_STRING("String offset cast occurred"));
				}
				break;
			case IS_LONG:
				break;
			default:
				zend_jit_error(
						llvm_ctx,
						opline,
						E_WARNING,
						"%s",
						LLVM_GET_CONST_STRING("Illegal offset type"));
				break;
		}
		if (convert) {
			lval = zval_get_long(RT_CONSTANT(llvm_ctx.op_array, *dim_op));
		}
		PHI_ADD(index, LLVM_GET_LONG(lval));
	} else {
		BasicBlock *bb_dim_long = NULL;
		BasicBlock *bb_dim_else = NULL;
		BasicBlock *bb_dim_finish = NULL;
		Value *index2 = NULL;

		if (dim_info & MAY_BE_LONG) {
			if (dim_info & (MAY_BE_ANY - MAY_BE_LONG)) {
				bb_dim_long = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);;
				bb_dim_else = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				bb_dim_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				if (!dim_type) {
					dim_type = zend_jit_load_type(llvm_ctx, dim, dim_ssa, dim_info);
				}
				// JIT: if (Z_TYPE_P(dim) != IS_LONG) 
				zend_jit_expected_br(llvm_ctx,
					llvm_ctx.builder.CreateICmpEQ(
						dim_type,
						llvm_ctx.builder.getInt8(IS_LONG)),
					bb_dim_long,
					bb_dim_else);
				llvm_ctx.builder.SetInsertPoint(bb_dim_long);
			}

			PHI_ADD(index, zend_jit_load_lval_c(llvm_ctx, dim, dim_op_type, dim_op, dim_ssa, dim_info));
			if (bb_dim_finish) {
				llvm_ctx.builder.CreateBr(bb_dim_finish);
			}
		}

		if (dim_info & (MAY_BE_ANY - MAY_BE_LONG)) {
			if (bb_dim_else) {
				llvm_ctx.builder.SetInsertPoint(bb_dim_else);
			}

			PHI_ADD(index, zend_jit_slow_str_index(llvm_ctx, dim, llvm_ctx.builder.getInt32(fetch_type), opline));

			if (bb_dim_finish) {
				llvm_ctx.builder.CreateBr(bb_dim_finish);
			} 
		}

		if (bb_dim_finish) {
			llvm_ctx.builder.SetInsertPoint(bb_dim_finish);
		}
	}

	PHI_SET(index, str_index, LLVM_GET_LONG_TY(llvm_ctx.context));

	if (container) {
		Value *refcount;
		Value *container_type_info = zend_jit_load_type_info(llvm_ctx, container, container_ssa, container_info);
		BasicBlock *bb_refounted = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_cont = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

		zend_jit_expected_br(
				llvm_ctx,
				llvm_ctx.builder.CreateICmpNE(
					llvm_ctx.builder.CreateAnd(
						container_type_info,
						llvm_ctx.builder.getInt32(IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT)),
					llvm_ctx.builder.getInt32(0)),
				bb_refounted,
				bb_cont);

		llvm_ctx.builder.SetInsertPoint(bb_refounted);
		if (container_info & MAY_BE_RCN) {
			BasicBlock *bb_rcn = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			BasicBlock *bb_rc1 = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			refcount = zend_jit_load_counted(llvm_ctx, container, container_ssa, container_info);
			zend_jit_unexpected_br(llvm_ctx,				
					llvm_ctx.builder.CreateICmpSGT(
						llvm_ctx.builder.CreateAlignedLoad(
							zend_jit_refcount_addr(llvm_ctx, refcount), 4),
						llvm_ctx.builder.getInt32(1)),
					bb_rcn,
					bb_rc1);
			llvm_ctx.builder.SetInsertPoint(bb_rcn);
			zend_jit_delref(llvm_ctx, zend_jit_load_counted(llvm_ctx, container, container_ssa, container_info));
			//???update reg value?
			zend_jit_copy_ctor_func(llvm_ctx, container, opline->lineno);
			llvm_ctx.builder.CreateBr(bb_rc1);
			llvm_ctx.builder.SetInsertPoint(bb_rc1);
		}

		refcount = zend_jit_load_counted(llvm_ctx, container, container_ssa, container_info);
		zend_jit_addref(llvm_ctx, refcount);

		llvm_ctx.builder.CreateBr(bb_cont);
		llvm_ctx.builder.SetInsertPoint(bb_cont);
	}

	return str_index;
}
/* }}} */

/* {{{ static Value* zend_jit_fetch_dimension_address_read */
static Value* zend_jit_fetch_dimension_address_read(zend_llvm_ctx     &llvm_ctx,
                                                    zend_class_entry  *scope,
                                                    Value             *container,
                                                    int                container_ssa,
                                                    uint32_t           container_info,
                                                    Value             *dim,
                                                    int                dim_ssa,
                                                    uint32_t           dim_info,
                                                    znode_op          *dim_op,
                                                    uint32_t           dim_op_type,
                                                    uint32_t           fetch_type,
                                                    zend_op           *opline,
                                                    zend_bool         *may_threw)
{
	Value *retval = NULL;
	Value *container_type = NULL;
	Value *new_element = NULL;
	BasicBlock *bb_error = NULL;
	BasicBlock *bb_uninitialized = NULL;
	BasicBlock *bb_new_element = NULL;
	BasicBlock *bb_follow = NULL;
	BasicBlock *bb_finish = NULL;
	PHI_DCL(ret, 10);

	container = zend_jit_deref(llvm_ctx, container, container_ssa, container_info);
	if ((container_info & MAY_BE_ARRAY)) {
		Value *item;

		if ((container_info & (MAY_BE_ANY - MAY_BE_ARRAY))) {
			BasicBlock *bb_array = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			if (!container_type) {
				container_type = zend_jit_load_type(llvm_ctx, container, container_ssa, container_info);
			}
			zend_jit_expected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(
					container_type,
					llvm_ctx.builder.getInt8(IS_ARRAY)),
				bb_array,
				bb_follow);
			llvm_ctx.builder.SetInsertPoint(bb_array);
		}

		item = zend_jit_fetch_dimension_address_inner(
				llvm_ctx,
				zend_jit_load_array_ht(
					llvm_ctx,
					zend_jit_load_array(llvm_ctx, container, container_ssa, container_info)),
				container_info,
				dim,
				NULL,
				dim_ssa,
				dim_info,
				dim_op_type,
				dim_op,
				&new_element,
				&bb_new_element,
				&bb_uninitialized,
				&bb_error,
				fetch_type,
				opline);

		if (item) {
			zend_jit_try_addref(llvm_ctx, item, NULL, IS_VAR, NULL, -1, MAY_BE_ANY);
			PHI_ADD(ret, item);
			if (!bb_finish) {
				bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(bb_finish);
		}
	
		if (bb_new_element) {
			llvm_ctx.builder.SetInsertPoint(bb_new_element);
			PHI_ADD(ret, new_element);
			if (!bb_finish) {
				bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(bb_finish);
		}

		if (bb_error) {
			llvm_ctx.builder.SetInsertPoint(bb_error);
			PHI_ADD(ret, llvm_ctx._EG_error_zval);
			if (!bb_finish) {
				bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(bb_finish);
		}

		if (bb_uninitialized) {
			llvm_ctx.builder.SetInsertPoint(bb_uninitialized);
			PHI_ADD(ret, llvm_ctx._EG_uninitialized_zval);
			if (!bb_finish) {
				bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(bb_finish);
		}
	}

	if ((container_info & MAY_BE_STRING)) {
		Value *index, *tmp, *str, *c;
		BasicBlock *bb_string;
		PHI_DCL(one_char, 2);

		if (bb_follow) {
			llvm_ctx.builder.SetInsertPoint(bb_follow);
			bb_follow = NULL;
		}

		if ((container_info & (MAY_BE_ANY - (MAY_BE_ARRAY|MAY_BE_STRING)))) {
			bb_string = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			if (!container_type) {
				container_type = zend_jit_load_type(llvm_ctx, container, container_ssa, container_info);
			}
			zend_jit_expected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(
					container_type,
					llvm_ctx.builder.getInt8(IS_STRING)),
				bb_string,
				bb_follow);
			llvm_ctx.builder.SetInsertPoint(bb_string);
		} else {
			bb_string = llvm_ctx.builder.GetInsertBlock();
		}

		index = zend_jit_str_offset_index(
			llvm_ctx,
			NULL,
			-1,
			-1,
			dim,
			dim_ssa,
			dim_info,
			dim_op_type,
			dim_op,
			fetch_type,
			opline);

		tmp = zend_jit_get_stack_slot(llvm_ctx, 0);
		str = zend_jit_load_str(llvm_ctx, container, container_ssa, container_info);
		// JIT: if (Z_LVAL_P(dim) < 0 || Z_STRLEN_P(container) <= Z_LVAL_P(dim))
		BasicBlock *ret_empty = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *ret_char = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *ret_cont = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		zend_jit_unexpected_br(llvm_ctx,
			llvm_ctx.builder.CreateICmpSLT(
				index,
				LLVM_GET_LONG(0)),
			ret_empty,
			ret_cont);

		llvm_ctx.builder.SetInsertPoint(ret_cont);
		zend_jit_unexpected_br(llvm_ctx,
			llvm_ctx.builder.CreateICmpSGE(
				index,
				zend_jit_load_str_len(llvm_ctx, str)),
			ret_empty,
			ret_char);

		// JIT: Empty string
		llvm_ctx.builder.SetInsertPoint(ret_empty);

		if (fetch_type != BP_VAR_IS) {
			zend_jit_error(
				llvm_ctx,
				opline,
				E_NOTICE,
				"Uninitialized string offset: %ld", 
				index);

			zend_jit_save_zval_str(llvm_ctx, tmp, -1, MAY_BE_ANY, llvm_ctx._CG_empty_string);
			zend_jit_save_zval_type_info(llvm_ctx, tmp, -1, MAY_BE_ANY, llvm_ctx.builder.getInt32(IS_INTERNED_STRING_EX));
		} else {
			zend_jit_save_zval_type_info(llvm_ctx, tmp, -1, MAY_BE_ANY, llvm_ctx.builder.getInt32(IS_NULL));
		}

		PHI_ADD(ret, tmp);
		if (!bb_finish) {
			bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		}
		llvm_ctx.builder.CreateBr(bb_finish);

		// JIT: Single char 
		llvm_ctx.builder.SetInsertPoint(ret_char);

#if 0 
		c = llvm_ctx.builder.CreateAlignedLoad(
				zend_jit_load_str_val(llvm_ctx, 
					zend_jit_load_str(llvm_ctx, container, container_ssa, container_info)), 1);

		BasicBlock *bb_cg_char = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_normal_char = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_do_ret = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

		zend_jit_expected_br(llvm_ctx,
				llvm_ctx.builder.CreateIsNotNull(
					llvm_ctx.builder.CreateAlignedLoad(
						llvm_ctx.builder.CreateGEP(
							llvm_ctx._CG_one_char_string,
							c), 4)),
				bb_cg_char,
				bb_normal_char);

		llvm_ctx.builder.SetInsertPoint(bb_cg_char);
		str = llvm_ctx.builder.CreateAlignedLoad(
				llvm_ctx.builder.CreateGEP(
					llvm_ctx._CG_one_char_string,
					c), 4);
		PHI_ADD(one_char, str);
		llvm_ctx.builder.CreateBr(bb_do_ret);

		llvm_ctx.builder.SetInsertPoint(bb_normal_char);
		str = zend_jit_string_alloc(llvm_ctx, LLVM_GET_LONG(1));

		llvm_ctx.builder.CreateAlignedStore(
			llvm_ctx.builder.CreateAlignedLoad(
				llvm_ctx.builder.CreateGEP(
					zend_jit_load_str_val(llvm_ctx, container),
					index),
				1),
			llvm_ctx.builder.CreateBitCast(
				zend_jit_load_str_val(llvm_ctx, str),
				PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context))),
			1);

		llvm_ctx.builder.CreateAlignedStore(
			llvm_ctx.builder.getInt8(0),
			LLVM_CREATE_CONST_GEP1(
				llvm_ctx.builder.CreateBitCast(
					zend_jit_load_str_val(llvm_ctx, str),
					PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context))),
				1), 4);

		PHI_ADD(one_char, str);
		llvm_ctx.builder.CreateBr(bb_do_ret);

		llvm_ctx.builder.SetInsertPoint(bb_do_ret);

		PHI_SET(one_char, str, PointerType::getUnqual(llvm_ctx.zend_string_type));
#endif
		str = zend_jit_string_alloc(llvm_ctx, LLVM_GET_LONG(1));

		llvm_ctx.builder.CreateAlignedStore(
			llvm_ctx.builder.CreateAlignedLoad(
				llvm_ctx.builder.CreateGEP(
					zend_jit_load_str_val(llvm_ctx,
						zend_jit_load_str(llvm_ctx, container, container_ssa, container_info)),
					index),
				1),
			llvm_ctx.builder.CreateBitCast(
				zend_jit_load_str_val(llvm_ctx, str),
				PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context))),
			1);

		llvm_ctx.builder.CreateAlignedStore(
			llvm_ctx.builder.getInt8(0),
			LLVM_CREATE_CONST_GEP1(
				llvm_ctx.builder.CreateBitCast(
					zend_jit_load_str_val(llvm_ctx, str),
					PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context))),
				1), 4);

		zend_jit_save_zval_str(llvm_ctx, tmp, -1, MAY_BE_ANY, str);
		zend_jit_save_zval_type_info(llvm_ctx, tmp, -1, MAY_BE_ANY, llvm_ctx.builder.getInt32(IS_STRING_EX));
		PHI_ADD(ret, tmp);
		if (!bb_finish) {
			bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		}
		llvm_ctx.builder.CreateBr(bb_finish);
	}

	if ((container_info & MAY_BE_OBJECT)) {
		Value *obj;
		Value *handlers;
		Value *read_dim_handler;

		*may_threw = 1;
		if (bb_follow) {
			llvm_ctx.builder.SetInsertPoint(bb_follow);
			bb_follow = NULL;
		}
		if ((container_info & (MAY_BE_ANY - (MAY_BE_ARRAY|MAY_BE_STRING|MAY_BE_OBJECT)))) {
			BasicBlock *bb_object = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			if (!container_type) {
				container_type = zend_jit_load_type(llvm_ctx, container, container_ssa, container_info);
			}
			zend_jit_expected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(
					container_type,
					llvm_ctx.builder.getInt8(IS_OBJECT)),
				bb_object,
				bb_follow);
			llvm_ctx.builder.SetInsertPoint(bb_object);
		}

		obj = zend_jit_load_obj(llvm_ctx, container, container_ssa, container_info);

		if (IS_CUSTOM_HANDLERS(scope)) {
			BasicBlock *bb_no_handler = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			BasicBlock *bb_read_dimension = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

			// JIT: if (!Z_OBJ_HT_P(container)->read_dimension)
			handlers = zend_jit_load_obj_handlers(llvm_ctx, obj);
			read_dim_handler = zend_jit_load_obj_handler(llvm_ctx, handlers, read_dimension);

			zend_jit_unexpected_br(
					llvm_ctx,
					llvm_ctx.builder.CreateIsNull(read_dim_handler),
					bb_no_handler,
					bb_read_dimension);

			llvm_ctx.builder.SetInsertPoint(bb_no_handler);
			zend_jit_error_noreturn(llvm_ctx, opline, E_ERROR, "%s",
					LLVM_GET_CONST_STRING("Cannot use object as array"));

			llvm_ctx.builder.SetInsertPoint(bb_read_dimension);
		} else {
			// JIT: zval *zend_std_read_dimension(zval *object, zval *offset, int type, zval *rv TSRMLS_DC)
			read_dim_handler = zend_jit_get_helper(
					llvm_ctx,
					(void*)std_object_handlers.read_dimension,
					ZEND_JIT_SYM("zend_std_read_dimension"),
					0,
					llvm_ctx.zval_ptr_type,
					llvm_ctx.zval_ptr_type,
					llvm_ctx.zval_ptr_type,
					Type::getInt32Ty(llvm_ctx.context),
					llvm_ctx.zval_ptr_type,
					NULL);
		}

		Value *rzv = zend_jit_get_stack_slot(llvm_ctx, 0);
		if (dim_info & MAY_BE_IN_REG) {
			dim = zend_jit_reload_from_reg(llvm_ctx, dim_ssa, dim_info);
		}
		Value *rv = zend_jit_read_dimension(llvm_ctx, read_dim_handler, container, dim, fetch_type, rzv, opline);

		BasicBlock *bb_found = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_not_found = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		zend_jit_expected_br(
			llvm_ctx,
			llvm_ctx.builder.CreateIsNotNull(rv),
			bb_found,
			bb_not_found);

		llvm_ctx.builder.SetInsertPoint(bb_found);

		BasicBlock *bb_same = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_not_same = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

		zend_jit_unexpected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(
					rzv, rv),
				bb_same,
				bb_not_same);

		llvm_ctx.builder.SetInsertPoint(bb_not_same);
		zend_jit_try_addref(llvm_ctx, rv, NULL, IS_VAR, NULL, -1, MAY_BE_ANY);
		llvm_ctx.builder.CreateBr(bb_same);
		llvm_ctx.builder.SetInsertPoint(bb_same);

		PHI_ADD(ret, rv);
		if (!bb_finish) {
			bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		}
		llvm_ctx.builder.CreateBr(bb_finish);

		llvm_ctx.builder.SetInsertPoint(bb_not_found);
		PHI_ADD(ret, llvm_ctx._EG_uninitialized_zval);
		if (!bb_finish) {
			bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		}
		llvm_ctx.builder.CreateBr(bb_finish);
	}

	if (bb_follow || (container_info & (MAY_BE_ANY - (MAY_BE_ARRAY|MAY_BE_STRING|MAY_BE_OBJECT)))) {
		if (bb_follow) {
			llvm_ctx.builder.SetInsertPoint(bb_follow);
		}
		PHI_ADD(ret, llvm_ctx._EG_uninitialized_zval);
		if (!bb_finish) {
			bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		}
		llvm_ctx.builder.CreateBr(bb_finish);
	}

	if (bb_finish) {
		llvm_ctx.builder.SetInsertPoint(bb_finish);
		PHI_SET(ret, retval, llvm_ctx.zval_ptr_type);
	}

	return retval;
}
/* }}} */

/* {{{ static Value* zend_jit_fetch_dimension_address */
static Value* zend_jit_fetch_dimension_address(zend_llvm_ctx     &llvm_ctx,
                                               zend_class_entry  *scope,
                                               Value             *result,
                                               Value             *container,
                                               Value             *container_type,
                                               int                container_ssa,
                                               uint32_t           container_info,
                                               znode_op          *container_op,
                                               uint32_t           container_op_type,
                                               Value             *dim,
                                               int                dim_ssa,
                                               uint32_t           dim_info,
                                               znode_op          *dim_op,
                                               uint32_t           dim_op_type,
                                               zend_uchar         fetch_type,
                                               Value            **offset,
                                               BasicBlock       **bb_str_offset,
                                               Value            **new_element,
                                               BasicBlock       **bb_new_element,
                                               Value            **object_property,
                                               BasicBlock       **bb_object_property,
                                               BasicBlock       **bb_uninitialized,
                                               BasicBlock       **bb_error,
                                               int                allow_str_offset,
                                               zend_op           *opline,
                                               zend_bool         *may_threw)
{
	Value *ret = NULL;
	BasicBlock *bb_convert_to_array = NULL;
	BasicBlock *bb_add_new = NULL;
	BasicBlock *bb_skip = NULL;
	BasicBlock *bb_follow = NULL;
	BasicBlock *bb_finish = NULL;
	BasicBlock *bb_collect = NULL;
	PHI_DCL(ret, 4);
	PHI_DCL(new_elem, 5);
	PHI_DCL(ind, 5);
	
	//JIT: if (EXPECTED(Z_TYPE_P(container) == IS_ARRAY))
	if ((container_info & (MAY_BE_ARRAY))) {
		if ((container_info & (MAY_BE_ANY - MAY_BE_ARRAY))) {
			BasicBlock *bb_array = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

			if (!container_type) {
				container_type = zend_jit_load_type(llvm_ctx, container, container_ssa, container_info);
			}

			zend_jit_expected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(
					container_type,
					llvm_ctx.builder.getInt8(IS_ARRAY)),
				bb_array,
				bb_follow);
			llvm_ctx.builder.SetInsertPoint(bb_array);
		}

		//JIT: SEPARATE_ARRAY(container)
		zend_jit_separate_array(
				llvm_ctx,
				container,
				NULL,
				container_op_type,
				container_op,
				container_ssa,
				container_info,
				opline);

		//JIT: if (dim == NULL)
		if (dim_op_type == IS_UNUSED) {
			BasicBlock *bb_fail = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			BasicBlock *bb_cont = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			Value *rv = zend_jit_hash_next_index_insert(
					llvm_ctx, 
					zend_jit_load_array_ht(
						llvm_ctx,
						zend_jit_load_array(llvm_ctx, container, container_ssa, container_info)),
					llvm_ctx._EG_uninitialized_zval,
					opline);
			zend_jit_unexpected_br(llvm_ctx,
					llvm_ctx.builder.CreateIsNull(rv),
					bb_fail,
					bb_cont);
			llvm_ctx.builder.SetInsertPoint(bb_fail);
			zend_jit_error(
					llvm_ctx,
					opline,
					E_WARNING,
					"%s",
					LLVM_GET_CONST_STRING("Cannot add element to the array as the next element is already occupied"));
			if (!*bb_error) {
				*bb_error = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(*bb_error);

			llvm_ctx.builder.SetInsertPoint(bb_cont);
			PHI_ADD(new_elem, rv);
			if (!bb_add_new) {
				bb_add_new = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(bb_add_new);
		} else {
			BasicBlock *bb_new = NULL;
			Value *new_elem;
			// JIT: retval = zend_fetch_dimension_address_inner(Z_ARRVAL_P(container), dim, dim_type, type TSRMLS_CC);
			Value *rv = zend_jit_fetch_dimension_address_inner(
					llvm_ctx,
					zend_jit_load_array_ht(
						llvm_ctx,
						zend_jit_load_array(llvm_ctx, container, container_ssa, container_info)),
					container_info,
					dim,
					NULL,
					dim_ssa,
					dim_info,
					dim_op_type,
					dim_op,
					&new_elem,
					&bb_new,
					bb_uninitialized,
					bb_error,
					fetch_type,
					opline);

			if (rv) {
				PHI_ADD(ret, rv);

				if (!bb_finish) {
					bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				}
				llvm_ctx.builder.CreateBr(bb_finish);
			}

			if (bb_new) {
				llvm_ctx.builder.SetInsertPoint(bb_new);
				PHI_ADD(new_elem, new_elem);
				if (!bb_add_new) {
					bb_add_new = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				}
				llvm_ctx.builder.CreateBr(bb_add_new);
			}
		}
	}

	//JIT: else if (EXPECTED(Z_TYPE_P(container) == IS_STRING)
	if (container_info & MAY_BE_STRING) {
		if (bb_follow) {
			llvm_ctx.builder.SetInsertPoint(bb_follow);
			bb_follow = NULL;
		}

		if ((container_info & (MAY_BE_ANY - (MAY_BE_STRING|MAY_BE_ARRAY)))) {
			BasicBlock *bb_string = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

			if (!container_type) {
				container_type = zend_jit_load_type(llvm_ctx, container, container_ssa, container_info);
			}

			zend_jit_expected_br(llvm_ctx,
					llvm_ctx.builder.CreateICmpEQ(
						container_type,
						llvm_ctx.builder.getInt8(IS_STRING)),
					bb_string,
					bb_follow);

			llvm_ctx.builder.SetInsertPoint(bb_string);
		}

		if (fetch_type != BP_VAR_UNSET) {
			// JIT: Z_STRLEN_P(container)==0
			if (!bb_convert_to_array) {
				bb_convert_to_array = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			bb_skip = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			zend_jit_unexpected_br(llvm_ctx,
					llvm_ctx.builder.CreateICmpEQ(
						zend_jit_load_str_len(llvm_ctx,
							zend_jit_load_str(llvm_ctx, container, container_ssa, container_info)),
						LLVM_GET_LONG(0)),
					bb_convert_to_array,
					bb_skip);
			llvm_ctx.builder.SetInsertPoint(bb_skip);
		}

		if (dim_op_type == IS_UNUSED) {
			// JIT: zend_error_noreturn(E_ERROR, "[] operator not supported for strings");
			zend_jit_error_noreturn(llvm_ctx, 
					opline,
					E_ERROR,
					"%s",
					LLVM_GET_CONST_STRING("[] operator not supported for strings"));
		} else {
			Value *index = zend_jit_str_offset_index(
					llvm_ctx,
					container,
					container_ssa,
					container_info,
					dim,
					dim_ssa,
					dim_info,
					dim_op_type,
					dim_op,
					fetch_type,
					opline);
			if (allow_str_offset) {
				*offset = index;
			} 
			if (!*bb_str_offset) {
				*bb_str_offset = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(*bb_str_offset);
		}
	}

	// JIT: case IS_OBJECT
	if (container_info & MAY_BE_OBJECT) {
		Value *obj = NULL;
		Value *handlers;
		Value *read_dim_handler;
		Value *property = dim;

		*may_threw = 1;
		if (bb_follow) {
			llvm_ctx.builder.SetInsertPoint(bb_follow);
			bb_follow = NULL;
		}
		if ((container_info & (MAY_BE_ANY - (MAY_BE_OBJECT|MAY_BE_STRING|MAY_BE_ARRAY)))) {
			BasicBlock *bb_object = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

			if (!container_type) {
				container_type = zend_jit_load_type(llvm_ctx, container, container_ssa, container_info);
			}

			zend_jit_expected_br(llvm_ctx,
					llvm_ctx.builder.CreateICmpEQ(
						container_type,
						llvm_ctx.builder.getInt8(IS_OBJECT)),
					bb_object,
					bb_follow);
			llvm_ctx.builder.SetInsertPoint(bb_object);
		}

		if (IS_CUSTOM_HANDLERS(scope)) {
			BasicBlock *bb_read_dimension = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			BasicBlock *bb_no_handler = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

			// JIT: if (!Z_OBJ_HT_P(container)->read_dimension)
			obj = zend_jit_load_obj(llvm_ctx, container, container_ssa, container_info);
			handlers = zend_jit_load_obj_handlers(llvm_ctx, obj);
			read_dim_handler = zend_jit_load_obj_handler(llvm_ctx, handlers, read_dimension);

			zend_jit_unexpected_br(
					llvm_ctx,
					llvm_ctx.builder.CreateIsNull(read_dim_handler),
					bb_no_handler,
					bb_read_dimension);

			llvm_ctx.builder.SetInsertPoint(bb_no_handler);
			zend_jit_error_noreturn(llvm_ctx, opline, E_ERROR, "%s",
					LLVM_GET_CONST_STRING("Cannot use object as array"));

			llvm_ctx.builder.SetInsertPoint(bb_read_dimension);
		} else {
			// JIT: zval *zend_std_read_dimension(zval *object, zval *offset, int type, zval *rv TSRMLS_DC)
			read_dim_handler = zend_jit_get_helper(
					llvm_ctx,
					(void*)std_object_handlers.read_dimension,
					ZEND_JIT_SYM("zend_std_read_dimension"),
					0,
					llvm_ctx.zval_ptr_type,
					llvm_ctx.zval_ptr_type,
					llvm_ctx.zval_ptr_type,
					Type::getInt32Ty(llvm_ctx.context),
					llvm_ctx.zval_ptr_type,
					NULL);
		}
		
		if (dim_op_type == IS_UNUSED) {
			dim = llvm_ctx.builder.CreateIntToPtr(LLVM_GET_LONG(0), llvm_ctx.zval_ptr_type);
		}

		{
			BasicBlock *bb_success = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			BasicBlock *bb_fail = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			BasicBlock *bb_ind = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			if (dim_info & MAY_BE_IN_REG) {
				dim = zend_jit_reload_from_reg(llvm_ctx, dim_ssa, dim_info);
			}
			Value *rv = zend_jit_read_dimension(llvm_ctx, read_dim_handler, container, dim, fetch_type, result, opline);
			Value *call_ret = zend_jit_slow_fetch_address_obj(llvm_ctx, container, rv, result, opline);

			zend_jit_unexpected_br(
					llvm_ctx,
					llvm_ctx.builder.CreateICmpSLE(
						call_ret,
						llvm_ctx.builder.getInt32(0)),
					bb_fail,
					bb_success);
			llvm_ctx.builder.SetInsertPoint(bb_fail);

			if (!*bb_uninitialized) {
				*bb_uninitialized = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}

			if (!*bb_error) {
				*bb_error = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}

			zend_jit_unexpected_br(
					llvm_ctx,
					llvm_ctx.builder.CreateICmpEQ(
						call_ret,
						llvm_ctx.builder.getInt32(0)),
					*bb_uninitialized,
					*bb_error);

			llvm_ctx.builder.SetInsertPoint(bb_success);

			*object_property = rv;
			if (!*bb_object_property) {
				*bb_object_property = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			zend_jit_unexpected_br(
					llvm_ctx,
					llvm_ctx.builder.CreateICmpEQ(
						call_ret,
						llvm_ctx.builder.getInt32(2)),
					bb_ind,
					*bb_object_property);
			llvm_ctx.builder.SetInsertPoint(bb_ind);
			PHI_ADD(ret, rv);
		}

		if (!bb_finish) {
			bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		}
		llvm_ctx.builder.CreateBr(bb_finish);
	}
	
	// JIT: case IS_NULL
	if ((container_info & MAY_BE_NULL)) {
		if (bb_follow) {
			llvm_ctx.builder.SetInsertPoint(bb_follow);
			bb_follow = NULL;
		}

		if ((container_info & (MAY_BE_ANY - (MAY_BE_NULL|MAY_BE_ARRAY|MAY_BE_OBJECT|MAY_BE_STRING)))) {
			BasicBlock *bb_null = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

			if (!container_type) {
				container_type = zend_jit_load_type(llvm_ctx, container, container_ssa, container_info);
			}

			zend_jit_expected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(
					container_type,
					llvm_ctx.builder.getInt8(IS_NULL)),
				bb_null,
				bb_follow);
			llvm_ctx.builder.SetInsertPoint(bb_null);
		}

		if (container_op_type == IS_VAR && (container_info & MAY_BE_ERROR)) {
			//JIT: if (UNEXPECTED(container == &EG(error_zval)))
			if (fetch_type != BP_VAR_UNSET) {
				if (!bb_convert_to_array) {
					bb_convert_to_array = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				}
			} else {
				if (!*bb_error) {
			    	*bb_error = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				}
			}
			if (!*bb_uninitialized) {
				*bb_uninitialized = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			zend_jit_expected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpNE(
					container,
					llvm_ctx._EG_error_zval),
				(fetch_type != BP_VAR_UNSET)? bb_convert_to_array : *bb_error,
				*bb_uninitialized);
		} else if (fetch_type != BP_VAR_UNSET) {
			if (!bb_convert_to_array) {
				bb_convert_to_array = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(bb_convert_to_array);
		} else {
			if (!*bb_uninitialized) {
				*bb_uninitialized = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(*bb_uninitialized);
		}
	}

	if (fetch_type != BP_VAR_UNSET) {
		if (container_info & MAY_BE_FALSE) {
			if (bb_follow) {
				llvm_ctx.builder.SetInsertPoint(bb_follow);
				bb_follow = NULL;
			}

			if (!bb_convert_to_array) {
				bb_convert_to_array = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			if ((container_info & (MAY_BE_ANY - (MAY_BE_FALSE|MAY_BE_NULL|MAY_BE_ARRAY|MAY_BE_OBJECT|MAY_BE_STRING)))) {
				bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

				if (!container_type) {
					container_type = zend_jit_load_type(llvm_ctx, container, container_ssa, container_info);
				}

				zend_jit_expected_br(llvm_ctx,
						llvm_ctx.builder.CreateICmpEQ(
							container_type,
							llvm_ctx.builder.getInt8(IS_FALSE)),
						bb_convert_to_array,
						bb_follow);
			} else {
				llvm_ctx.builder.CreateBr(bb_convert_to_array);
			}
		}
	}

	if (bb_follow
		|| (container_info
	       & (MAY_BE_ANY - ((fetch_type == BP_VAR_UNSET)?
			   (MAY_BE_ARRAY|MAY_BE_NULL|MAY_BE_STRING|MAY_BE_OBJECT) :
			   (MAY_BE_ARRAY|MAY_BE_NULL|MAY_BE_STRING|MAY_BE_OBJECT|MAY_BE_FALSE))))) {
		if (bb_follow) {
			llvm_ctx.builder.SetInsertPoint(bb_follow);
		}

		if (fetch_type == BP_VAR_UNSET) {
			zend_jit_error(
					llvm_ctx,
					opline,
					E_WARNING, "%s",
					LLVM_GET_CONST_STRING("Cannot unset offset in a non-array variable"));
			if (!*bb_uninitialized) {
				*bb_uninitialized = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(*bb_uninitialized);
		} else {
			zend_jit_error(
					llvm_ctx,
					opline,
					E_WARNING, "%s",
					LLVM_GET_CONST_STRING("Cannot use a scalar value as an array"));
			if (!*bb_error) {
				*bb_error = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(*bb_error);
		}
	} 
   
	if (bb_convert_to_array) {
		Value *rv;
		llvm_ctx.builder.SetInsertPoint(bb_convert_to_array);

		zend_jit_init_array(llvm_ctx, container, 8);

		if (dim_op_type == IS_UNUSED) {
			rv = zend_jit_hash_next_index_insert(
					llvm_ctx, 
					zend_jit_load_array_ht(
						llvm_ctx,
						zend_jit_load_array(llvm_ctx, container, container_ssa, container_info)),
					llvm_ctx._EG_uninitialized_zval,
					opline);
			PHI_ADD(new_elem, rv);
			if (!bb_add_new) {
				bb_add_new = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(bb_add_new);
		} else {
			Value *new_elem;
			BasicBlock *bb_new = NULL;
			rv = zend_jit_fetch_dimension_address_inner(
					llvm_ctx,
					zend_jit_load_array_ht(
						llvm_ctx,
						zend_jit_load_array(llvm_ctx, container, container_ssa, container_info)),
					0,
					dim,
					NULL,
					dim_ssa,
					dim_info,
					dim_op_type,
					dim_op,
					&new_elem,
					&bb_new,
					bb_uninitialized,
					bb_error,
					fetch_type,
					opline);

			if (rv) {
				PHI_ADD(ret, rv);
				if (!bb_finish) {
					bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				}
				llvm_ctx.builder.CreateBr(bb_finish);
			}

			if (bb_new) {
				llvm_ctx.builder.SetInsertPoint(bb_new);
				PHI_ADD(new_elem, new_elem);
				if (!bb_add_new) {
					bb_add_new = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				}
				llvm_ctx.builder.CreateBr(bb_add_new);
			}
		}
	}

	if (bb_add_new) {
		llvm_ctx.builder.SetInsertPoint(bb_add_new);
		PHI_SET(new_elem, *new_element, llvm_ctx.zval_ptr_type);

		if (!*bb_new_element) {
			*bb_new_element = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		}
		llvm_ctx.builder.CreateBr(*bb_new_element);	
	}

	if (bb_finish) {
		llvm_ctx.builder.SetInsertPoint(bb_finish);
		PHI_SET(ret, ret, llvm_ctx.zval_ptr_type);
	}

	return ret;
}
/* }}} */

/* {{{ static int zend_jit_concat_function */
static int zend_jit_concat_function(zend_llvm_ctx      &llvm_ctx,
                                    zend_class_entry   *op1_scope,    
                                    Value              *op1_orig_addr,
                                    Value              *op1_addr,
                                    int                 op1_ssa_var,
                                    uint32_t            op1_info,
                                    zend_uchar          op1_op_type,
                                    znode_op           *op1_op,
                                    zend_class_entry   *op2_scope,
                                    Value              *op2_orig_addr,
                                    Value              *op2_addr,
                                    int                 op2_ssa_var,
                                    uint32_t            op2_info,
                                    zend_uchar          op2_op_type,
                                    znode_op           *op2_op,
                                    Value              *result_addr,
                                    int                 result_ssa_var,
                                    uint32_t            result_info,
                                    zend_uchar          result_op_type,
                                    znode_op           *result_op,
                                    zend_op            *opline,
                                    zend_bool          *may_threw)
{
	Value *op1_str;
	Value *op2_str;
	Value *op1_type = NULL;
	Value *op2_type = NULL;
	Value *op1_copy_str = NULL;
	Value *op2_copy_str = NULL;
	BasicBlock *bb_finish = NULL;
	BasicBlock *bb_follow = NULL;
	BasicBlock *bb_do_concat = NULL;
	BasicBlock *bb_op1_string = NULL;
	BasicBlock *bb_op1_copy = NULL;
	BasicBlock *bb_op2_string = NULL;
	BasicBlock *bb_op2_copy = NULL;
	PHI_DCL(op1_copy, 8);
	PHI_DCL(op2_copy, 8);
	PHI_DCL(op1_str, 8);
	PHI_DCL(op2_str, 8);

	//JIT: ZEND_TRY_BINARY_OBJECT_OPERATION(opcode)
	if (op1_info & MAY_BE_OBJECT) {
		if (IS_CUSTOM_HANDLERS(op1_scope)) {
			if (op1_info & (MAY_BE_ANY - MAY_BE_OBJECT)) {
				Value  *obj;
				Value  *handlers;
				Value  *do_op;
				Value  *op_ret;
				BasicBlock *bb_object = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				BasicBlock *bb_has_do_op = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				op1_type = zend_jit_load_type(llvm_ctx, op1_addr, op1_ssa_var, op1_info);

				zend_jit_expected_br(llvm_ctx,
						llvm_ctx.builder.CreateICmpEQ(
							op1_type,
							llvm_ctx.builder.getInt8(IS_OBJECT)),
						bb_object,
						bb_follow);
				llvm_ctx.builder.SetInsertPoint(bb_object);
	
				obj = zend_jit_load_obj(llvm_ctx, op1_addr, op1_ssa_var, op1_info);
				handlers = zend_jit_load_obj_handlers(llvm_ctx, obj);
				do_op = zend_jit_load_obj_handler(llvm_ctx, handlers, do_operation);
		
				zend_jit_unexpected_br(llvm_ctx,
						llvm_ctx.builder.CreateIsNotNull(do_op),
					bb_has_do_op,
					bb_follow);

				llvm_ctx.builder.SetInsertPoint(bb_has_do_op);

				Value *op1_real_addr;
				Value *op2_real_addr;
				if (op1_info & MAY_BE_IN_REG) {
					op1_real_addr = zend_jit_reload_from_reg(llvm_ctx, op1_ssa_var, op1_info);
				} else {
					op1_real_addr = op1_addr;
				}

				if (op2_info & MAY_BE_IN_REG) {
					op2_real_addr = zend_jit_reload_from_reg(llvm_ctx, op2_ssa_var, op2_info);
				} else {
					op2_real_addr = op2_addr;
				}

				op_ret = zend_jit_do_operation(
						llvm_ctx,
						do_op,
						opline->opcode,
						result_addr,
						op1_real_addr,
						op2_real_addr,
						opline);

				bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				zend_jit_expected_br(
						llvm_ctx,
						llvm_ctx.builder.CreateICmpEQ(
							op_ret,
							llvm_ctx.builder.getInt32(0)),
						bb_finish,
						bb_follow);
			}
		}
	}

	if (op2_info & MAY_BE_OBJECT) {
		if (IS_CUSTOM_HANDLERS(op2_scope)) {
			if (bb_follow) {
				llvm_ctx.builder.SetInsertPoint(bb_follow);
				bb_follow = NULL;
			}

			if (op2_info & (MAY_BE_ANY - MAY_BE_OBJECT)) {
				Value  *obj;
				Value  *handlers;
				Value  *do_op;
				Value  *op_ret;
				BasicBlock *bb_object = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				BasicBlock *bb_has_do_op = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				op2_type = zend_jit_load_type(llvm_ctx, op2_addr, op2_ssa_var, op2_info);

				zend_jit_expected_br(llvm_ctx,
						llvm_ctx.builder.CreateICmpEQ(
							op2_type,
							llvm_ctx.builder.getInt8(IS_OBJECT)),
						bb_object,
						bb_follow);
				llvm_ctx.builder.SetInsertPoint(bb_object);
	
				obj = zend_jit_load_obj(llvm_ctx, op2_addr, op2_ssa_var, op2_info);
				handlers = zend_jit_load_obj_handlers(llvm_ctx, obj);
				do_op = zend_jit_load_obj_handler(llvm_ctx, handlers, do_operation);
		
				zend_jit_unexpected_br(llvm_ctx,
						llvm_ctx.builder.CreateIsNotNull(do_op),
					bb_has_do_op,
					bb_follow);

				llvm_ctx.builder.SetInsertPoint(bb_has_do_op);

				op_ret = zend_jit_do_operation(
						llvm_ctx,
						do_op,
						opline->opcode,
						result_addr,
						op1_addr,
						op2_addr,
						opline);

				if (!bb_finish) {
					bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				}

				zend_jit_expected_br(
						llvm_ctx,
						llvm_ctx.builder.CreateICmpEQ(
							op_ret,
							llvm_ctx.builder.getInt32(0)),
						bb_finish,
						bb_follow);
			}
		}
	}

	if (bb_follow) {
		llvm_ctx.builder.SetInsertPoint(bb_follow);
		bb_follow = NULL;
	}

	zend_jit_make_printable_zval(
			llvm_ctx,
			op1_addr,
			&op1_type,
			op1_op_type,
			op1_op,
			op1_ssa_var,
			op1_info,
			&bb_op1_string,
			&op1_str,
			&bb_op1_copy,
			&op1_copy_str,
			opline);

	if (bb_op1_string) {
		llvm_ctx.builder.SetInsertPoint(bb_op1_string);
		if (op1_info & MAY_BE_IN_REG) {
			op1_addr = zend_jit_reload_from_reg(llvm_ctx, op1_ssa_var, op1_info);
		}

		if (op2_info & MAY_BE_IN_REG) {
			op2_addr = zend_jit_reload_from_reg(llvm_ctx, op2_ssa_var, op2_info);
		}

		if (op1_addr == op2_addr) {
			PHI_ADD(op1_copy, llvm_ctx.builder.getInt8(0));
			PHI_ADD(op2_copy, llvm_ctx.builder.getInt8(0));
			PHI_ADD(op1_str, op1_str);
			PHI_ADD(op2_str, op1_str);

			if (!bb_do_concat) {
				bb_do_concat = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(bb_do_concat);
		} else {
			BasicBlock *bb_same_op = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			BasicBlock *bb_cont = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			op2_type = NULL;

			zend_jit_unexpected_br(
					llvm_ctx,
					llvm_ctx.builder.CreateICmpEQ(
						op2_addr,
						op1_addr),
					bb_same_op,
					bb_cont);

			llvm_ctx.builder.SetInsertPoint(bb_same_op);

			PHI_ADD(op1_copy, llvm_ctx.builder.getInt8(0));
			PHI_ADD(op2_copy, llvm_ctx.builder.getInt8(0));
			PHI_ADD(op1_str, op1_str);
			PHI_ADD(op2_str, op1_str);
			if (!bb_do_concat) {
				bb_do_concat = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(bb_do_concat);

			llvm_ctx.builder.SetInsertPoint(bb_cont);

			zend_jit_make_printable_zval(
					llvm_ctx,
					op2_addr,
					&op2_type,
					op2_op_type,
					op2_op,
					op2_ssa_var,
					op2_info,
					&bb_op2_string,
					&op2_str,
					&bb_op2_copy,
					&op2_copy_str,
					opline);

			if (bb_op2_string) {
				llvm_ctx.builder.SetInsertPoint(bb_op2_string);
				PHI_ADD(op1_copy, llvm_ctx.builder.getInt8(0));
				PHI_ADD(op2_copy, llvm_ctx.builder.getInt8(0));
				PHI_ADD(op1_str, op1_str);
				PHI_ADD(op2_str, op2_str);
				if (!bb_do_concat) {
					bb_do_concat = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				}
				llvm_ctx.builder.CreateBr(bb_do_concat);
			}

			if (bb_op2_copy) {
				llvm_ctx.builder.SetInsertPoint(bb_op2_copy);
				PHI_ADD(op1_copy, llvm_ctx.builder.getInt8(0));
				PHI_ADD(op2_copy, llvm_ctx.builder.getInt8(1));
				PHI_ADD(op1_str, zend_jit_load_str(llvm_ctx, op1_addr, op1_ssa_var, op1_info));
				PHI_ADD(op2_str, op2_copy_str);
				if (!bb_do_concat) {
					bb_do_concat = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				}
				llvm_ctx.builder.CreateBr(bb_do_concat);
			}
		}
	}

	if (bb_op1_copy) {
		BasicBlock *bb_cont = NULL;
			
		llvm_ctx.builder.SetInsertPoint(bb_op1_copy);

		if (op1_info & MAY_BE_IN_REG) {
			op1_addr = zend_jit_reload_from_reg(llvm_ctx, op1_ssa_var, op1_info);
		}

		if (op2_info & MAY_BE_IN_REG) {
			op2_addr = zend_jit_reload_from_reg(llvm_ctx, op2_ssa_var, op2_info);
		}

		if (result_info & MAY_BE_IN_REG) {
			result_addr = zend_jit_reload_from_reg(llvm_ctx, result_ssa_var, result_info);
		}

		if (op1_addr == result_addr) {
			zend_jit_zval_dtor_ex(llvm_ctx, op1_addr, op1_type, op1_ssa_var, op1_info, opline->lineno);
			if (op1_addr != op2_addr) {
				BasicBlock *bb_same_op = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				bb_cont = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

				zend_jit_unexpected_br(
						llvm_ctx,
						llvm_ctx.builder.CreateICmpEQ(
							op1_addr,
							op2_addr),
						bb_same_op,
						bb_cont);
				llvm_ctx.builder.SetInsertPoint(bb_same_op);
			}

			PHI_ADD(op1_copy, llvm_ctx.builder.getInt8(1));
			PHI_ADD(op2_copy, llvm_ctx.builder.getInt8(0));
			PHI_ADD(op1_str, op1_copy_str);
			PHI_ADD(op2_str, op1_copy_str);

			if (!bb_do_concat) {
				bb_do_concat = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(bb_do_concat);
		} else if (result_addr) {
			BasicBlock *bb_same_op = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_cont = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

			zend_jit_unexpected_br(
					llvm_ctx,
					llvm_ctx.builder.CreateICmpEQ(
						result_addr,
						op1_addr),
					bb_same_op,
					bb_cont);

			llvm_ctx.builder.SetInsertPoint(bb_same_op);
			zend_jit_zval_dtor_ex(llvm_ctx, op1_addr, op1_type, op1_ssa_var, op1_info, opline->lineno);

			if (op1_addr != op2_addr) {
				bb_same_op = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

				zend_jit_unexpected_br(
						llvm_ctx,
						llvm_ctx.builder.CreateICmpEQ(
							result_addr,
							op2_addr),
						bb_same_op,
						bb_cont);
				llvm_ctx.builder.SetInsertPoint(bb_same_op);
			}

			PHI_ADD(op1_copy, llvm_ctx.builder.getInt8(1));
			PHI_ADD(op2_copy, llvm_ctx.builder.getInt8(0));
			PHI_ADD(op1_str, op1_copy_str);
			PHI_ADD(op2_str, op1_copy_str);
			if (!bb_do_concat) {
				bb_do_concat = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(bb_do_concat);
		} 

		if (bb_cont) {
			llvm_ctx.builder.SetInsertPoint(bb_cont);
			bb_op2_string = NULL;
			bb_op2_copy = NULL;
			op2_copy_str = NULL;
			op2_type = NULL;

			zend_jit_make_printable_zval(
					llvm_ctx,
					op2_addr,
					&op2_type,
					op2_op_type,
					op2_op,
					op2_ssa_var,
					op2_info,
					&bb_op2_string,
					&op2_str,
					&bb_op2_copy,
					&op2_copy_str,
					opline);

			if (bb_op2_string) {
				llvm_ctx.builder.SetInsertPoint(bb_op2_string);
				PHI_ADD(op1_copy, llvm_ctx.builder.getInt8(1));
				PHI_ADD(op2_copy, llvm_ctx.builder.getInt8(0));
				PHI_ADD(op1_str, op1_copy_str);
				PHI_ADD(op2_str, op2_str);
				if (!bb_do_concat) {
					bb_do_concat = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				}
				llvm_ctx.builder.CreateBr(bb_do_concat);
			}

			if (bb_op2_copy) {
				llvm_ctx.builder.SetInsertPoint(bb_op2_copy);
				PHI_ADD(op1_copy, llvm_ctx.builder.getInt8(1));
				PHI_ADD(op2_copy, llvm_ctx.builder.getInt8(1));
				PHI_ADD(op1_str, op1_copy_str);
				PHI_ADD(op2_str, op2_copy_str);
				if (!bb_do_concat) {
					bb_do_concat = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				}
				llvm_ctx.builder.CreateBr(bb_do_concat);
			}
		}
	}

	if (bb_do_concat) {
		Value *op1_str_len;
		Value *op2_str_len;
		Value *result_str;
		Value *result_str_len;
		Value *op1_use_copy;
		Value *op2_use_copy;
		Value *real_op2_str;
		BasicBlock *bb_overflow;
		BasicBlock *bb_same_op;
		BasicBlock *bb_cont;
		PHI_DCL(real_op2_str, 3);
		PHI_DCL(result_str, 3);

		llvm_ctx.builder.SetInsertPoint(bb_do_concat);

		PHI_SET(op1_copy, op1_use_copy, Type::getInt8Ty(llvm_ctx.context));
		PHI_SET(op2_copy, op2_use_copy, Type::getInt8Ty(llvm_ctx.context));
		PHI_SET(op1_str, op1_str, PointerType::getUnqual(llvm_ctx.zend_string_type));
		PHI_SET(op2_str, op2_str, PointerType::getUnqual(llvm_ctx.zend_string_type));

		op1_str_len = zend_jit_load_str_len(llvm_ctx, op1_str);
		op2_str_len = zend_jit_load_str_len(llvm_ctx, op2_str);
		
		bb_overflow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		//TODO: avoid this by ssa_var?
		zend_jit_unexpected_br(
				llvm_ctx,
				llvm_ctx.builder.CreateICmpUGT(
					op1_str_len,
					llvm_ctx.builder.CreateSub(
						LLVM_GET_LONG(SIZE_MAX),
						op2_str_len)),
				bb_overflow, 
				bb_follow);
		llvm_ctx.builder.SetInsertPoint(bb_overflow);
		zend_jit_error_noreturn(
				llvm_ctx,
				opline,
				E_ERROR,
				"%s",
				LLVM_GET_CONST_STRING("String size overflow"));
		llvm_ctx.builder.SetInsertPoint(bb_follow);

		result_str_len = llvm_ctx.builder.CreateAdd(op1_str_len, op2_str_len);

		bb_follow = NULL;
		if ((op1_op_type == result_op_type) && (op1_info & (MAY_BE_RC1|MAY_BE_RCN))) {
			Value *op1_type_info;
			BasicBlock *bb_refounted = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_cont = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

			if (result_addr != op1_addr && op1_addr) {
				bb_same_op = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				zend_jit_unexpected_br(
						llvm_ctx,
						llvm_ctx.builder.CreateICmpEQ(
							result_addr,
							op1_addr),
						bb_same_op,
						bb_cont);

				llvm_ctx.builder.SetInsertPoint(bb_same_op);
			}

			op1_type_info = zend_jit_load_type_info_c(llvm_ctx, op1_addr, op1_op_type, op1_op, op1_ssa_var, op1_info);
			zend_jit_unexpected_br(
					llvm_ctx,
					llvm_ctx.builder.CreateICmpNE(
						llvm_ctx.builder.CreateAnd(
							op1_type_info,
							llvm_ctx.builder.getInt32(IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT)),
						llvm_ctx.builder.getInt32(0)),
					bb_refounted,
					bb_cont);
			llvm_ctx.builder.SetInsertPoint(bb_refounted);

			//JIT: result_str = zend_string_realloc(Z_STR_P(result), result_len, 0);
			result_str = zend_jit_string_realloc(llvm_ctx, op1_str, result_str_len);
			if (op1_addr != op2_addr && op2_addr) {
				BasicBlock *bb_cont2 = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				bb_same_op = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				zend_jit_unexpected_br(
						llvm_ctx,
						llvm_ctx.builder.CreateICmpEQ(
							op2_addr,
							result_addr),
						bb_same_op,
						bb_cont2);
				llvm_ctx.builder.SetInsertPoint(bb_same_op);
				PHI_ADD(result_str, result_str);
				PHI_ADD(real_op2_str, result_str);
				llvm_ctx.builder.CreateBr(bb_follow);
				llvm_ctx.builder.SetInsertPoint(bb_cont2);
				PHI_ADD(result_str, result_str);
				PHI_ADD(real_op2_str, op2_str);
				llvm_ctx.builder.CreateBr(bb_follow);
			} else {
				PHI_ADD(result_str, result_str);
				PHI_ADD(real_op2_str, op2_str);
				llvm_ctx.builder.CreateBr(bb_follow);
			}
			llvm_ctx.builder.SetInsertPoint(bb_cont);
		}
		result_str = zend_jit_string_alloc(llvm_ctx, result_str_len);
		llvm_ctx.builder.CreateMemCpy(
				zend_jit_load_str_val(llvm_ctx, result_str),
				zend_jit_load_str_val(llvm_ctx, op1_str),
				op1_str_len, 1);

		PHI_ADD(result_str, result_str);
		if (bb_follow) {
			PHI_ADD(real_op2_str, op2_str);
			llvm_ctx.builder.CreateBr(bb_follow);
			llvm_ctx.builder.SetInsertPoint(bb_follow);
			PHI_SET(real_op2_str, real_op2_str, PointerType::getUnqual(llvm_ctx.zend_string_type));
		} else {
			real_op2_str = op2_str;
		}
			
		PHI_SET(result_str, result_str, PointerType::getUnqual(llvm_ctx.zend_string_type));

		llvm_ctx.builder.CreateMemCpy(
				llvm_ctx.builder.CreateGEP(
					zend_jit_load_str_val(llvm_ctx, result_str),
					op1_str_len),
				zend_jit_load_str_val(llvm_ctx, real_op2_str),
				op2_str_len, 1);

		llvm_ctx.builder.CreateAlignedStore(
				llvm_ctx.builder.getInt8(0),
				llvm_ctx.builder.CreateGEP(
					zend_jit_load_str_val(llvm_ctx, result_str),
					result_str_len), 1);

		zend_jit_save_zval_str(llvm_ctx, result_addr, result_ssa_var, result_info, result_str);
		zend_jit_save_zval_type_info(llvm_ctx, result_addr, result_ssa_var, result_info, llvm_ctx.builder.getInt32(IS_STRING_EX));

		if (op1_info & (MAY_BE_ANY - MAY_BE_STRING)) {
			bb_cont = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

			zend_jit_unexpected_br(
					llvm_ctx,
					llvm_ctx.builder.CreateICmpNE(
						op1_use_copy,
						llvm_ctx.builder.getInt8(0)),
					bb_cont,
					bb_follow);
			llvm_ctx.builder.SetInsertPoint(bb_cont);
			zend_jit_string_release(llvm_ctx, op1_str);
			llvm_ctx.builder.CreateBr(bb_follow);
			llvm_ctx.builder.SetInsertPoint(bb_follow);
		}

		if (op2_info & (MAY_BE_ANY - MAY_BE_STRING)) {
			bb_cont = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

			zend_jit_unexpected_br(
					llvm_ctx,
					llvm_ctx.builder.CreateICmpNE(
						op2_use_copy,
						llvm_ctx.builder.getInt8(0)),
					bb_cont,
					bb_follow);
			llvm_ctx.builder.SetInsertPoint(bb_cont);
			zend_jit_string_release(llvm_ctx, op2_str);
			llvm_ctx.builder.CreateBr(bb_follow);
			llvm_ctx.builder.SetInsertPoint(bb_follow);
		}

		if (bb_finish) {
			llvm_ctx.builder.CreateBr(bb_finish);
		}
	}

	if (bb_finish) {
		llvm_ctx.builder.SetInsertPoint(bb_finish);
	}

	if (op1_orig_addr && !zend_jit_free_operand(llvm_ctx, op1_op_type, op1_orig_addr, NULL, op1_ssa_var, op1_info, opline->lineno)) {
	   	return 0;
	}
	if (op2_orig_addr && !zend_jit_free_operand(llvm_ctx, op2_op_type, op2_orig_addr, NULL, op2_ssa_var, op2_info, opline->lineno)) {
	   	return 0;
	}

	return 1;
}
/* }}} */

/* {{{ static void zend_jit_assign_to_object */
static void zend_jit_assign_to_object(zend_llvm_ctx     &llvm_ctx,
                                      zend_class_entry  *scope,
                                      Value             *container,
                                      Value             *container_type,
                                      int                container_ssa_var,
                                      uint32_t           container_info,
                                      uint32_t           container_op_type,
                                      Value             *property,
                                      int                property_ssa_var,
                                      uint32_t           property_info,
                                      znode_op          *property_op,
                                      uint32_t           property_op_type,
									  Value             *result,
                                      int                result_ssa_var,
                                      uint32_t           result_info,
									  Value             *cache_slot,
                                      uint32_t           opcode,
                                      zend_op_array     *op_array,
                                      zend_op           *opline,
                                      zend_bool         *may_threw)
{
	Value *orig_value;
	Value *value;
	BasicBlock *bb_finish = NULL;
	BasicBlock *bb_follow = NULL;
	int value_ssa_var = OP1_DATA_SSA_VAR();
	int value_info = OP1_DATA_INFO();

	orig_value = zend_jit_load_operand(llvm_ctx, 
			OP1_DATA_OP_TYPE(), OP1_DATA_OP(), value_ssa_var, value_info, 0, opline);
	if (OP1_DATA_OP_TYPE() == IS_VAR || OP1_DATA_OP_TYPE() == IS_CV) {
		value = zend_jit_deref(llvm_ctx, orig_value, value_ssa_var, value_info);
	} else {
		value = orig_value;
	}

 	//JIT: ZVAL_DEREF(object);
	container = zend_jit_deref(llvm_ctx, container, container_ssa_var, container_info);

	//JIT: if (UNEXPECTED(Z_TYPE_P(object) != IS_OBJECT))
	if (container_info & (MAY_BE_ANY - MAY_BE_OBJECT)) {
		BasicBlock *bb_cont = NULL;
		BasicBlock *bb_convert_to_object = NULL;
		BasicBlock *bb_not_object = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

		if (!container_type) {
			container_type = zend_jit_load_type(llvm_ctx, container, container_ssa_var, container_info);
		}

		zend_jit_unexpected_br(
				llvm_ctx,
				llvm_ctx.builder.CreateICmpNE(
					container_type,
					llvm_ctx.builder.getInt8(IS_OBJECT)),
				bb_not_object,
				bb_follow);
		llvm_ctx.builder.SetInsertPoint(bb_not_object);

		//JIT: if (UNEXPECTED(object == &EG(error_zval)))
		if (container_info & MAY_BE_ERROR) {
			BasicBlock *bb_error = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_cont = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

			zend_jit_unexpected_br(
					llvm_ctx,
					llvm_ctx.builder.CreateICmpEQ(
						container,
						llvm_ctx._EG_error_zval),
					bb_error,
					bb_cont);
			llvm_ctx.builder.SetInsertPoint(bb_error);
			if (RETURN_VALUE_USED(opline)) {
				zend_jit_save_zval_type_info(llvm_ctx, result, result_ssa_var, result_info, llvm_ctx.builder.getInt32(IS_NULL));
			}
			//TODO: FREE_OP(free_value);
			if (!bb_finish) {
				bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(bb_finish);
		}

		if (container_info & MAY_BE_NULL) {
			if (bb_cont) {
				llvm_ctx.builder.SetInsertPoint(bb_cont);
				bb_cont = NULL;
			}

			if (container_info & (MAY_BE_ANY - (MAY_BE_OBJECT|MAY_BE_NULL))) {
				bb_cont = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

				if (!bb_convert_to_object) {
					bb_convert_to_object = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				}

				if (!container_type) {
					container_type = zend_jit_load_type(llvm_ctx, container, container_ssa_var, container_info);
				}

				zend_jit_expected_br(
						llvm_ctx,
						llvm_ctx.builder.CreateICmpEQ(
							container_type,
							llvm_ctx.builder.getInt8(IS_NULL)),
					bb_convert_to_object,
					bb_cont);
			} else {
				if (!bb_convert_to_object) {
					bb_convert_to_object = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				}
				llvm_ctx.builder.CreateBr(bb_convert_to_object);
			}
		}

		if (container_info & MAY_BE_FALSE) {
			if (bb_cont) {
				llvm_ctx.builder.SetInsertPoint(bb_cont);
				bb_cont = NULL;
			}

			if (container_info & (MAY_BE_ANY - MAY_BE_FALSE|MAY_BE_NULL|MAY_BE_OBJECT)) {
				bb_cont = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

				if (!bb_convert_to_object) {
					bb_convert_to_object = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				}

				if (!container_type) {
					container_type = zend_jit_load_type(llvm_ctx, container, container_ssa_var, container_info);
				}

				zend_jit_expected_br(
						llvm_ctx,
						llvm_ctx.builder.CreateICmpEQ(
							container_type,
							llvm_ctx.builder.getInt8(IS_FALSE)),
						bb_convert_to_object,
						bb_cont);
			} else {
				if (!bb_convert_to_object) {
					bb_convert_to_object = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				}
				llvm_ctx.builder.CreateBr(bb_convert_to_object);
			}
		}

		if (container_info & MAY_BE_STRING) {
			BasicBlock *bb_empty_str = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			if (bb_cont) {
				llvm_ctx.builder.SetInsertPoint(bb_cont);
			}

			bb_cont = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

			if (container_info & (MAY_BE_ANY - MAY_BE_STRING|MAY_BE_FALSE|MAY_BE_NULL|MAY_BE_OBJECT)) {
				BasicBlock *bb_next = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

				if (!bb_convert_to_object) {
					bb_convert_to_object = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				}

				if (!container_type) {
					container_type = zend_jit_load_type(llvm_ctx, container, container_ssa_var, container_info);
				}

				zend_jit_expected_br(
						llvm_ctx,
						llvm_ctx.builder.CreateICmpEQ(
							container_type,
							llvm_ctx.builder.getInt8(IS_STRING)),
						bb_next,
						bb_cont);
				llvm_ctx.builder.SetInsertPoint(bb_next);
			} 

			zend_jit_unexpected_br(
					llvm_ctx,
					llvm_ctx.builder.CreateICmpEQ(
						zend_jit_load_str_len(llvm_ctx,
							zend_jit_load_str(llvm_ctx, container, container_ssa_var, container_info)),
						LLVM_GET_LONG(0)),
					bb_empty_str,
					bb_cont);
			llvm_ctx.builder.SetInsertPoint(bb_empty_str);

			zend_jit_zval_ptr_dtor_ex(llvm_ctx, container, container_type, -1, MAY_BE_STRING, opline->lineno, 0);

			if (!bb_convert_to_object) {
				bb_convert_to_object = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}

			llvm_ctx.builder.CreateBr(bb_convert_to_object);
		}

		if (bb_cont) {
			llvm_ctx.builder.SetInsertPoint(bb_cont);
			//JIT: zend_error(E_WARNING, "Attempt to assign property of non-object");
			zend_jit_error(
					llvm_ctx,
					opline,
					E_WARNING,
					"%s",
					LLVM_GET_CONST_STRING("Attempt to assign property of non-object"));

			if (result) {
				zend_jit_save_zval_type_info(llvm_ctx, result, result_ssa_var, result_info, llvm_ctx.builder.getInt32(IS_NULL));
			}
			//TODO: FREE_OP(free_value);
			if (!bb_finish) {
				bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(bb_finish);
		}
		
		if (bb_convert_to_object) {
			Value *counted;
			BasicBlock *bb_release_obj;

			llvm_ctx.builder.SetInsertPoint(bb_convert_to_object);

			bb_release_obj = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_cont = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

			zend_jit_object_init(llvm_ctx, container, opline->lineno);
			counted = zend_jit_load_counted(llvm_ctx, container, container_ssa_var, container_info);
			zend_jit_addref(llvm_ctx, counted);

			zend_jit_error(
					llvm_ctx,
					opline,
					E_WARNING,
					"%s",
					LLVM_GET_CONST_STRING("Creating default object from empty value"));

			zend_jit_unexpected_br(llvm_ctx,
					llvm_ctx.builder.CreateICmpEQ(
						llvm_ctx.builder.CreateAlignedLoad(
							zend_jit_refcount_addr(llvm_ctx, counted), 4),
						llvm_ctx.builder.getInt32(1)),
					bb_release_obj,
					bb_cont);

			llvm_ctx.builder.SetInsertPoint(bb_release_obj);

			if (result) {
				zend_jit_save_zval_type_info(llvm_ctx, result, result_ssa_var, result_info, llvm_ctx.builder.getInt32(IS_NULL));
			}

			zend_jit_object_release(llvm_ctx, zend_jit_load_obj(llvm_ctx, container, container_ssa_var, container_info), opline->lineno);
			//TODO: FREE_OP(free_value);
			if (!bb_finish) {
				bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(bb_finish);
			llvm_ctx.builder.SetInsertPoint(bb_cont);

			zend_jit_delref(llvm_ctx, counted);

			if (bb_follow) {
				llvm_ctx.builder.CreateBr(bb_follow);
			}
		}
	}

	if (bb_follow) {
		llvm_ctx.builder.SetInsertPoint(bb_follow);
	}

	if (OP1_DATA_OP_TYPE() == IS_TMP_VAR) {
		/* ??? */
	} else if (OP1_DATA_OP_TYPE() == IS_CONST) {
		if (value_info & (MAY_BE_STRING|MAY_BE_ARRAY)) {
			Value *real_val;
			Value *val_info = zend_jit_load_type_info_c(llvm_ctx, value, IS_CONST, OP1_DATA_OP(), value_ssa_var, value_info);
			BasicBlock *bb_copy = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			BasicBlock *bb_cont = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			PHI_DCL(real_val, 2);

			PHI_ADD(real_val, value);
			zend_jit_unexpected_br(
					llvm_ctx,
					llvm_ctx.builder.CreateICmpNE(
						llvm_ctx.builder.CreateAnd(
							val_info,
							llvm_ctx.builder.getInt32((IS_TYPE_COPYABLE) << Z_TYPE_FLAGS_SHIFT)),
						llvm_ctx.builder.getInt32(0)),
					bb_copy,
					bb_cont);
			llvm_ctx.builder.SetInsertPoint(bb_copy);
			real_val = zend_jit_get_stack_slot(llvm_ctx, 0);
			zend_jit_copy_value(llvm_ctx,
				real_val,
				0,
				-1,
				MAY_BE_ANY,
				value, 
				NULL,
				IS_CONST,
				OP1_DATA_OP(),
				value_ssa_var,
				value_info);
			zend_jit_copy_ctor_func(llvm_ctx, real_val, opline->lineno);
			PHI_ADD(real_val, real_val);
			llvm_ctx.builder.CreateBr(bb_cont);
			llvm_ctx.builder.SetInsertPoint(bb_cont);
			PHI_SET(real_val, value, llvm_ctx.zval_ptr_type);
		}
		value_info |= MAY_BE_RC1 | MAY_BE_RCN;
	} else {
		zend_jit_try_addref(llvm_ctx, value, NULL, OP1_DATA_OP_TYPE(), OP1_DATA_OP(), value_ssa_var, value_info);
	}

	if (opcode == ZEND_ASSIGN_OBJ) {
		Value *write_property_handler;
		if (IS_CUSTOM_HANDLERS(scope)) {
			Value *obj;
			Value *handlers;
			BasicBlock *bb_write_property = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			BasicBlock *bb_no_handler = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

			// JIT: if (!Z_OBJ_HT_P(container)->write_property)
			obj = zend_jit_load_obj(llvm_ctx, container, container_ssa_var, container_info);
			handlers = zend_jit_load_obj_handlers(llvm_ctx, obj);
			write_property_handler = zend_jit_load_obj_handler(llvm_ctx, handlers, write_property);

			zend_jit_unexpected_br(
					llvm_ctx,
					llvm_ctx.builder.CreateIsNull(write_property_handler),
					bb_no_handler,
					bb_write_property);

			llvm_ctx.builder.SetInsertPoint(bb_no_handler);
			zend_jit_error(
					llvm_ctx,
					opline,
					E_WARNING,
					"%s",
					LLVM_GET_CONST_STRING("Attempt to assign property of non-object"));

			if (result) {
				zend_jit_save_zval_type_info(llvm_ctx, result, result_ssa_var, result_info, llvm_ctx.builder.getInt32(IS_NULL));
			}

			if (OP1_DATA_OP_TYPE() == IS_CONST) {
				zend_jit_zval_ptr_dtor_ex(llvm_ctx, value, NULL, value_ssa_var, value_info, opline->lineno, 0);
			}

			if (!bb_finish) {
				bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(bb_finish);

			llvm_ctx.builder.SetInsertPoint(bb_write_property);
		} else {
			// JIT: void zend_std_write_property(zval *object, zval *member, zval *value, void **cache_slot TSRMLS_DC)
			write_property_handler = zend_jit_get_helper(
					llvm_ctx,
					(void*)std_object_handlers.write_property,
					ZEND_JIT_SYM("zend_std_write_property"),
					0,
					PointerType::getVoidTy(llvm_ctx.context),
					llvm_ctx.zval_ptr_type,
					llvm_ctx.zval_ptr_type,
					llvm_ctx.zval_ptr_type,
					PointerType::getUnqual(
						PointerType::getUnqual(
							LLVM_GET_LONG_TY(llvm_ctx.context))),
					NULL);
		}

		zend_jit_write_property(llvm_ctx, write_property_handler, container, property, value, cache_slot, opline);
	} else {
		Value *write_dimension_handler;
		if (IS_CUSTOM_HANDLERS(scope)) {
			Value *obj;
			Value *handlers;
			BasicBlock *bb_write_dimension = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			BasicBlock *bb_no_handler = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

			// JIT: if (!Z_OBJ_HT_P(container)->write_dimension)
			obj = zend_jit_load_obj(llvm_ctx, container, container_ssa_var, container_info);
			handlers = zend_jit_load_obj_handlers(llvm_ctx, obj);
			write_dimension_handler = zend_jit_load_obj_handler(llvm_ctx, handlers, write_dimension);

			zend_jit_unexpected_br(
					llvm_ctx,
					llvm_ctx.builder.CreateIsNull(write_dimension_handler),
					bb_no_handler,
					bb_write_dimension);

			llvm_ctx.builder.SetInsertPoint(bb_no_handler);
			zend_jit_error_noreturn(
					llvm_ctx,
					opline,
					E_ERROR,
					"%s",
					LLVM_GET_CONST_STRING("Cannot use object as array"));

			llvm_ctx.builder.SetInsertPoint(bb_write_dimension);
		} else {
			// JIT: static void zend_std_write_dimension(zval *object, zval *offset, zval *value TSRMLS_DC)
			write_dimension_handler = zend_jit_get_helper(
					llvm_ctx,
					(void*)std_object_handlers.write_dimension,
					ZEND_JIT_SYM("zend_std_write_dimension"),
					0,
					PointerType::getVoidTy(llvm_ctx.context),
					llvm_ctx.zval_ptr_type,
					llvm_ctx.zval_ptr_type,
					llvm_ctx.zval_ptr_type,
					NULL,
					NULL);
		}
		zend_jit_write_dimension(llvm_ctx, write_dimension_handler, container, property, value, opline);
	}

	if (result /* && !EG(exception_op) */) {
		zend_jit_copy_value(llvm_ctx,
				result,
				0,
				result_ssa_var,
				result_info,
				value, 
				NULL,
				OP1_DATA_OP_TYPE(),
				OP1_DATA_OP(),
				value_ssa_var,
				value_info);
		zend_jit_try_addref(llvm_ctx, result, NULL, OP1_DATA_OP_TYPE(), OP1_DATA_OP(), value_ssa_var, value_info);
	}

	zend_jit_zval_ptr_dtor_ex(llvm_ctx, value, NULL, value_ssa_var, value_info, opline->lineno, 1);
	if (OP1_DATA_OP_TYPE() == IS_VAR) {
		if (!zend_jit_free_operand(llvm_ctx,
					OP1_DATA_OP_TYPE(), orig_value, NULL, value_ssa_var, value_info, opline->lineno)) {
			/* return 0; */
		}
	}

	if (bb_finish) {
		llvm_ctx.builder.CreateBr(bb_finish);
		llvm_ctx.builder.SetInsertPoint(bb_finish);
	}
}
/* }}} */

/* Handlers */

/* {{{ static int zend_jit_jmpznz */
static int zend_jit_jmpznz(zend_llvm_ctx    &llvm_ctx,
                           zend_op_array    *op_array,
                           zend_op          *opline,
                           BasicBlock       *bb_false,
                           BasicBlock       *bb_true,
                           int               expected_branch)
{
	Value *orig_zval_addr = NULL;
	Value *zval_addr = NULL;
	Value *zval_type = NULL;
	Value *zval_val  = NULL;
	BasicBlock *bb_follow;

	if (OP1_OP_TYPE() == IS_CONST) {
		// Convert to unconditional branch
		llvm_ctx.builder.CreateBr(
			zval_is_true(RT_CONSTANT(llvm_ctx.op_array, *OP1_OP())) ? bb_true : bb_false);
		return 1;
	} else if (!OP1_MAY_BE(MAY_BE_ANY - (MAY_BE_NULL | MAY_BE_FALSE))) {
		llvm_ctx.builder.CreateBr(bb_false);
		return 1;
	} else if (!OP1_MAY_BE(MAY_BE_ANY - MAY_BE_TRUE)) {
		llvm_ctx.builder.CreateBr(bb_true);
		return 1;
	}

	// JIT: val = GET_OP1_ZVAL_PTR_DEREF(BP_VAR_R)
	orig_zval_addr = zend_jit_load_operand(llvm_ctx, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO(), 1, opline);
	if (OP1_OP_TYPE() == IS_VAR || OP1_OP_TYPE() == IS_CV) {
		zval_addr = zend_jit_deref(llvm_ctx, orig_zval_addr, OP1_SSA_VAR(), OP1_INFO());
	} else {
		zval_addr = orig_zval_addr;
	}
	// JIT: type = Z_TYPE_P(val)
	zval_type = zend_jit_load_type(llvm_ctx, zval_addr, OP1_SSA_VAR(), OP1_INFO());
	if (OP1_MAY_BE(MAY_BE_TRUE)) {
		if (OP1_MAY_BE(MAY_BE_ANY - MAY_BE_TRUE)) {
			// JIT: if (type == IS_TRUE) goto bb_true;
			bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			zend_jit_unexpected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(
					zval_type,
					llvm_ctx.builder.getInt8(IS_TRUE)),
				bb_true,
				bb_follow);
			llvm_ctx.builder.SetInsertPoint(bb_follow);
		}
	}
	if (OP1_MAY_BE(MAY_BE_NULL | MAY_BE_FALSE)) {
		if (OP1_MAY_BE(MAY_BE_ANY - (MAY_BE_NULL | MAY_BE_FALSE | MAY_BE_TRUE))) {
			// JIT: if (type < IS_TRUE) goto bb_false;
			bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			zend_jit_unexpected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpULT(
					zval_type,
					llvm_ctx.builder.getInt8(IS_TRUE)),
				bb_false,
				bb_follow);
			llvm_ctx.builder.SetInsertPoint(bb_follow);
		} else {
			llvm_ctx.builder.CreateBr(bb_false);
		}
	}
	if (OP1_MAY_BE(MAY_BE_LONG)) {
		if (OP1_MAY_BE(MAY_BE_ANY - (MAY_BE_NULL | MAY_BE_FALSE | MAY_BE_TRUE | MAY_BE_LONG))) {
			// JIT: if (type == IS_LONG) goto bb_long;
			BasicBlock *bb_long = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			zend_jit_expected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(
					zval_type,
					llvm_ctx.builder.getInt8(IS_LONG)),
				bb_long,
				bb_follow);
			llvm_ctx.builder.SetInsertPoint(bb_long);
		}
		zend_jit_expected_br_ex(llvm_ctx,
			llvm_ctx.builder.CreateICmpNE(
				zend_jit_load_lval_c(llvm_ctx, zval_addr, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO()),
				LLVM_GET_LONG(0)),
			bb_true,
			bb_false,
			expected_branch);
		if (OP1_MAY_BE(MAY_BE_ANY - (MAY_BE_NULL | MAY_BE_FALSE | MAY_BE_TRUE | MAY_BE_LONG))) {
			llvm_ctx.builder.SetInsertPoint(bb_follow);
		}
	}
	if (OP1_MAY_BE(MAY_BE_DOUBLE)) {
		if (OP1_MAY_BE(MAY_BE_ANY - (MAY_BE_NULL | MAY_BE_FALSE | MAY_BE_TRUE | MAY_BE_LONG | MAY_BE_DOUBLE))) {
			// JIT: if (type == IS_DOUBLE) goto bb_double;
			BasicBlock *bb_double = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			zend_jit_expected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(
					zval_type,
					llvm_ctx.builder.getInt8(IS_DOUBLE)),
				bb_double,
				bb_follow);
			llvm_ctx.builder.SetInsertPoint(bb_double);
		}		
		zend_jit_expected_br_ex(llvm_ctx,
			llvm_ctx.builder.CreateFCmpUNE(
				zend_jit_load_dval(llvm_ctx, zval_addr, OP1_SSA_VAR(), OP1_INFO()),
				ConstantFP::get(Type::getDoubleTy(llvm_ctx.context), 0.0)),
			bb_true,
			bb_false,
			expected_branch);
		if (OP1_MAY_BE(MAY_BE_ANY - (MAY_BE_NULL | MAY_BE_FALSE | MAY_BE_TRUE | MAY_BE_LONG | MAY_BE_DOUBLE))) {
			llvm_ctx.builder.SetInsertPoint(bb_follow);
		}
	}
	if (OP1_MAY_BE(MAY_BE_ANY - (MAY_BE_NULL | MAY_BE_FALSE | MAY_BE_TRUE | MAY_BE_LONG | MAY_BE_DOUBLE))) {
		Value *cmp = llvm_ctx.builder.CreateICmpNE(
				zend_jit_is_true(llvm_ctx, zval_addr),
				llvm_ctx.builder.getInt32(0));
		if (!zend_jit_free_operand(llvm_ctx, OP1_OP_TYPE(), orig_zval_addr, zval_type, OP1_SSA_VAR(), OP1_INFO() & ~(MAY_BE_NULL | MAY_BE_FALSE | MAY_BE_TRUE | MAY_BE_LONG | MAY_BE_DOUBLE), opline->lineno)) {
			return 0;
		}
		zend_jit_expected_br_ex(llvm_ctx, cmp,
			bb_true,
			bb_false,
			expected_branch);
	}
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_math_dispatch */
static int zend_jit_math_dispatch(zend_llvm_ctx     &llvm_ctx,
                                  Value             *op1_addr,
                                  zend_uchar         op1_op_type,
                                  znode_op          *op1_op,
                                  int                op1_ssa_var,
                                  uint32_t           op1_info,
                                  Value             *op2_addr,
                                  zend_uchar         op2_op_type,
                                  znode_op          *op2_op,
                                  int                op2_ssa_var,
                                  uint32_t           op2_info,
                                  zend_bool          same_cvs,
                                  BasicBlock       **bb_long_long,
                                  BasicBlock       **bb_long_double,
                                  BasicBlock       **bb_double_long,
                                  BasicBlock       **bb_double_double,
                                  BasicBlock       **bb_slow_path)
{
	int n = 0;
	Value *op1_type = NULL;
	Value *op2_type = NULL;

	*bb_long_long = NULL;
	*bb_long_double = NULL;
	*bb_double_long = NULL;
	*bb_double_double = NULL;
	*bb_slow_path = NULL;

	if ((op1_info & MAY_BE_LONG) && (op2_info & MAY_BE_LONG)) {
		n++;
	}
	if (!same_cvs && (op1_info & MAY_BE_LONG) && (op2_info & MAY_BE_DOUBLE)) {
		n++;
	}
	if (!same_cvs && (op1_info & MAY_BE_DOUBLE) && (op2_info & MAY_BE_LONG)) {
		n++;
	}
	if ((op1_info & MAY_BE_DOUBLE) && (op2_info & MAY_BE_DOUBLE)) {
		n++;
	}
	if ((op1_info & (MAY_BE_ANY-(MAY_BE_LONG|MAY_BE_DOUBLE))) ||
	    (op2_info & (MAY_BE_ANY-(MAY_BE_LONG|MAY_BE_DOUBLE)))) {
		n++;
	}

	if (n == 1) {
		if ((op1_info & MAY_BE_LONG) && (op2_info & MAY_BE_LONG)) {
			*bb_long_long = llvm_ctx.builder.GetInsertBlock();
		}
		if (!same_cvs && (op1_info & MAY_BE_LONG) && (op2_info & MAY_BE_DOUBLE)) {
			*bb_long_double = llvm_ctx.builder.GetInsertBlock();
		}
		if (!same_cvs && (op1_info & MAY_BE_DOUBLE) && (op2_info & MAY_BE_LONG)) {
			*bb_double_long = llvm_ctx.builder.GetInsertBlock();
		}
		if ((op1_info & MAY_BE_DOUBLE) && (op2_info & MAY_BE_DOUBLE)) {
			*bb_double_double = llvm_ctx.builder.GetInsertBlock();
		}
		if ((op1_info & (MAY_BE_ANY-(MAY_BE_LONG|MAY_BE_DOUBLE))) ||
		    (op2_info & (MAY_BE_ANY-(MAY_BE_LONG|MAY_BE_DOUBLE)))) {
			*bb_slow_path = llvm_ctx.builder.GetInsertBlock();
		}
		return 1;
	} else {
		if ((op1_info & MAY_BE_LONG) && (op2_info & MAY_BE_LONG)) {
			*bb_long_long = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		}
		if (!same_cvs && (op1_info & MAY_BE_LONG) && (op2_info & MAY_BE_DOUBLE)) {
			*bb_long_double = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		}
		if (!same_cvs && (op1_info & MAY_BE_DOUBLE) && (op2_info & MAY_BE_LONG)) {
			*bb_double_long = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		}
		if ((op1_info & MAY_BE_DOUBLE) && (op2_info & MAY_BE_DOUBLE)) {
			*bb_double_double = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		}
		if ((op1_info & (MAY_BE_ANY-(MAY_BE_LONG|MAY_BE_DOUBLE))) ||
		    (op2_info & (MAY_BE_ANY-(MAY_BE_LONG|MAY_BE_DOUBLE)))) {
			*bb_slow_path = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		}
	}

	if ((op1_info & MAY_BE_LONG) && (op2_info & MAY_BE_LONG)) {
		BasicBlock *bb_op1_no_int = NULL;

		if (op1_info & (MAY_BE_ANY-MAY_BE_LONG)) {
			BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_op1_no_int = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			op1_type = zend_jit_load_type_c(llvm_ctx, op1_addr, op1_op_type, op1_op, op1_ssa_var, op1_info);
			zend_jit_expected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(
					op1_type,
					llvm_ctx.builder.getInt8(IS_LONG)),
				bb_follow,
				bb_op1_no_int);
			llvm_ctx.builder.SetInsertPoint(bb_follow);
		}
		if (!same_cvs && (op2_info & (MAY_BE_ANY-MAY_BE_LONG))) {
			BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			op2_type = zend_jit_load_type_c(llvm_ctx, op2_addr, op2_op_type, op2_op, op2_ssa_var, op2_info);
			zend_jit_expected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(
					op2_type,
					llvm_ctx.builder.getInt8(IS_LONG)),
				*bb_long_long,
				bb_follow);
			llvm_ctx.builder.SetInsertPoint(bb_follow);
			if (op2_info & MAY_BE_DOUBLE) {
				if (op2_info & (MAY_BE_ANY-(MAY_BE_LONG|MAY_BE_DOUBLE))) {
					zend_jit_expected_br(llvm_ctx,
						llvm_ctx.builder.CreateICmpEQ(
							op2_type,
							llvm_ctx.builder.getInt8(IS_DOUBLE)),
						*bb_long_double,
						*bb_slow_path);
				} else {
					llvm_ctx.builder.CreateBr(*bb_long_double);
				}
			} else {
				llvm_ctx.builder.CreateBr(*bb_slow_path);
			}
		} else {
			llvm_ctx.builder.CreateBr(*bb_long_long);
		}
		if (bb_op1_no_int) {
			op2_type = NULL;
			llvm_ctx.builder.SetInsertPoint(bb_op1_no_int);
			if (op1_info & MAY_BE_DOUBLE) {
				if (op1_info & (MAY_BE_ANY-(MAY_BE_LONG|MAY_BE_DOUBLE))) {
					BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					zend_jit_expected_br(llvm_ctx,
						llvm_ctx.builder.CreateICmpEQ(
							op1_type,
							llvm_ctx.builder.getInt8(IS_DOUBLE)),
						bb_follow,
						*bb_slow_path);
					llvm_ctx.builder.SetInsertPoint(bb_follow);
				}
				if (op2_info & MAY_BE_DOUBLE) {
					if (!same_cvs && (op2_info & (MAY_BE_ANY-MAY_BE_DOUBLE))) {
						BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
						op2_type = zend_jit_load_type_c(llvm_ctx, op2_addr, op2_op_type, op2_op, op2_ssa_var, op2_info);
						zend_jit_expected_br(llvm_ctx,
							llvm_ctx.builder.CreateICmpEQ(
								op2_type,
								llvm_ctx.builder.getInt8(IS_DOUBLE)),
							*bb_double_double,
							bb_follow);
						llvm_ctx.builder.SetInsertPoint(bb_follow);
					} else {
						llvm_ctx.builder.CreateBr(*bb_double_double);
					}
				}
				if (!same_cvs) {
					if (op2_info & MAY_BE_LONG) {
						if (!op2_type) {
							op2_type = zend_jit_load_type_c(llvm_ctx, op2_addr, op2_op_type, op2_op, op2_ssa_var, op2_info);
						}
						if (op2_info & (MAY_BE_ANY-(MAY_BE_LONG|MAY_BE_DOUBLE))) {
							zend_jit_expected_br(llvm_ctx,
								llvm_ctx.builder.CreateICmpEQ(
									op2_type,
									llvm_ctx.builder.getInt8(IS_LONG)),
								*bb_double_long,
								*bb_slow_path);
						} else {
							llvm_ctx.builder.CreateBr(*bb_double_long);
						}
					} else {
						llvm_ctx.builder.CreateBr(*bb_slow_path);
					}
				}
			} else {
				llvm_ctx.builder.CreateBr(*bb_slow_path);
			}
		}
	} else if ((op1_info & MAY_BE_DOUBLE) &&
	           !(op1_info & MAY_BE_LONG) &&
	           (op2_info & (MAY_BE_LONG|MAY_BE_DOUBLE))) {
		if (op1_info & (MAY_BE_ANY-MAY_BE_DOUBLE)) {
			BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			op1_type = zend_jit_load_type_c(llvm_ctx, op1_addr, op1_op_type, op1_op, op1_ssa_var, op1_info);
			zend_jit_expected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(
					op1_type,
					llvm_ctx.builder.getInt8(IS_DOUBLE)),
				bb_follow,
				*bb_slow_path);
			llvm_ctx.builder.SetInsertPoint(bb_follow);
		}
		if (op2_info & MAY_BE_DOUBLE) {
			if (!same_cvs && (op2_info & (MAY_BE_ANY-MAY_BE_DOUBLE))) {
				BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				op2_type = zend_jit_load_type_c(llvm_ctx, op2_addr, op2_op_type, op2_op, op2_ssa_var, op2_info);
				zend_jit_expected_br(llvm_ctx,
					llvm_ctx.builder.CreateICmpEQ(
						op2_type,
						llvm_ctx.builder.getInt8(IS_DOUBLE)),
					*bb_double_double,
					bb_follow);
				llvm_ctx.builder.SetInsertPoint(bb_follow);
			} else {
				llvm_ctx.builder.CreateBr(*bb_double_double);
			}
		}
		if (!same_cvs && (op2_info & MAY_BE_LONG)) {
			if (op2_info & (MAY_BE_ANY-(MAY_BE_DOUBLE|MAY_BE_LONG))) {
				if (!op2_type) {
					op2_type = zend_jit_load_type_c(llvm_ctx, op2_addr, op2_op_type, op2_op, op2_ssa_var, op2_info);
				}
				zend_jit_expected_br(llvm_ctx,
					llvm_ctx.builder.CreateICmpEQ(
						op2_type,
						llvm_ctx.builder.getInt8(IS_LONG)),
					*bb_double_long,
					*bb_slow_path);
			} else {
				llvm_ctx.builder.CreateBr(*bb_double_long);
			}
		} else if (!same_cvs && (op2_info & (MAY_BE_ANY-(MAY_BE_LONG|MAY_BE_DOUBLE)))) {
			llvm_ctx.builder.CreateBr(*bb_slow_path);
		}
	} else if ((op2_info & MAY_BE_DOUBLE) &&
	           !(op2_info & MAY_BE_LONG) &&
	           (op1_info & (MAY_BE_LONG|MAY_BE_DOUBLE))) {
		if (op2_info & (MAY_BE_ANY-MAY_BE_DOUBLE)) {
			BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			op2_type = zend_jit_load_type_c(llvm_ctx, op2_addr, op2_op_type, op2_op, op2_ssa_var, op2_info);
			zend_jit_expected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(
					op2_type,
					llvm_ctx.builder.getInt8(IS_DOUBLE)),
				bb_follow,
				*bb_slow_path);
			llvm_ctx.builder.SetInsertPoint(bb_follow);
		}
		if (op1_info & MAY_BE_DOUBLE) {
			if (!same_cvs && (op1_info & (MAY_BE_ANY-MAY_BE_DOUBLE))) {
				BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				op1_type = zend_jit_load_type_c(llvm_ctx, op1_addr, op1_op_type, op1_op, op1_ssa_var, op1_info);
				zend_jit_expected_br(llvm_ctx,
					llvm_ctx.builder.CreateICmpEQ(
						op1_type,
						llvm_ctx.builder.getInt8(IS_DOUBLE)),
					*bb_double_double,
					bb_follow);
				llvm_ctx.builder.SetInsertPoint(bb_follow);
			} else {
				llvm_ctx.builder.CreateBr(*bb_double_double);
			}
		}
		if (op1_info & MAY_BE_LONG) {
			if (op1_info & (MAY_BE_ANY-(MAY_BE_DOUBLE|MAY_BE_LONG))) {
				if (!op1_type) {
					op1_type = zend_jit_load_type_c(llvm_ctx, op1_addr, op1_op_type, op1_op, op1_ssa_var, op1_info);
				}
				zend_jit_expected_br(llvm_ctx,
					llvm_ctx.builder.CreateICmpEQ(
						op1_type,
						llvm_ctx.builder.getInt8(IS_LONG)),
					*bb_long_double,
					*bb_slow_path);
			} else {
				llvm_ctx.builder.CreateBr(*bb_long_double);
			}
		} else if (op1_info & (MAY_BE_ANY-(MAY_BE_LONG|MAY_BE_DOUBLE))) {
			llvm_ctx.builder.CreateBr(*bb_slow_path);
		}
	} else if ((op1_info & (MAY_BE_ANY-(MAY_BE_LONG|MAY_BE_DOUBLE))) ||
	           (op2_info & (MAY_BE_ANY-(MAY_BE_LONG|MAY_BE_DOUBLE)))) {
		llvm_ctx.builder.CreateBr(*bb_slow_path);
	}

	return n;
}
/* }}} */

/* {{{ static int zend_jit_cmp */
static int zend_jit_cmp(zend_llvm_ctx    &llvm_ctx,
                        zend_op_array    *op_array,
                        zend_op          *opline,
                        BasicBlock       *bb_false,
                        BasicBlock       *bb_true,
                        int               expected_branch)
{
	Value *orig_op1_addr = NULL;
	Value *orig_op2_addr = NULL;
	Value *op1_addr = NULL;
	Value *op2_addr = NULL;
	Value *op1_val = NULL;
	Value *op2_val = NULL;
	Value *op1_val1 = NULL;
	Value *op2_val1 = NULL;
	Value *op1_val2 = NULL;
	Value *op2_val2 = NULL;
	Value *cmp;
	BasicBlock *bb_long_long;
	BasicBlock *bb_long_double;
	BasicBlock *bb_double_long;
	BasicBlock *bb_double_double;
	BasicBlock *bb_slow_path;
	BasicBlock *bb_finish = NULL;
	BasicBlock *bb_double_double_cvt = NULL;
	int n;

	// Select optimal operand loading order
	if (!zend_jit_load_operands(llvm_ctx, op_array, opline, &orig_op1_addr, &orig_op2_addr)) return 0;
	if (OP1_OP_TYPE() == IS_VAR || OP1_OP_TYPE() == IS_CV) {
		op1_addr = zend_jit_deref(llvm_ctx, orig_op1_addr, OP1_SSA_VAR(), OP1_INFO());
	} else {
		op1_addr = orig_op1_addr;
	}
	if (OP2_OP_TYPE() == IS_VAR || OP2_OP_TYPE() == IS_CV) {
		op2_addr = zend_jit_deref(llvm_ctx, orig_op2_addr, OP2_SSA_VAR(), OP2_INFO());
	} else {
		op2_addr = orig_op2_addr;
	}

	n = zend_jit_math_dispatch(llvm_ctx,
			op1_addr,
			OP1_OP_TYPE(),
			OP1_OP(),
			OP1_SSA_VAR(),
			OP1_INFO(),
			op2_addr,
			OP2_OP_TYPE(),
			OP2_OP(),
			OP2_SSA_VAR(),
			OP2_INFO(),
			SAME_CVs(opline),
			&bb_long_long, 
			&bb_long_double,
			&bb_double_long,
			&bb_double_double,
			&bb_slow_path);

	if (bb_true && bb_false) {
		n = 0;
	}
	if (n > 1) {
		bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	}

	if (bb_long_long) {
		llvm_ctx.builder.SetInsertPoint(bb_long_long);
		op1_val = zend_jit_load_lval_c(llvm_ctx, op1_addr, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO());
		op2_val = zend_jit_load_lval_c(llvm_ctx, op2_addr, OP2_OP_TYPE(), OP2_OP(), OP2_SSA_VAR(), OP2_INFO());
		if (opline->opcode == ZEND_IS_EQUAL || opline->opcode == ZEND_CASE || opline->opcode == ZEND_IS_IDENTICAL) {
			cmp = llvm_ctx.builder.CreateICmpEQ(op1_val, op2_val);
		} else if (opline->opcode == ZEND_IS_NOT_EQUAL || opline->opcode == ZEND_IS_NOT_IDENTICAL) {
			cmp = llvm_ctx.builder.CreateICmpNE(op1_val, op2_val);
		} else if (opline->opcode == ZEND_IS_SMALLER) {
			cmp = llvm_ctx.builder.CreateICmpSLT(op1_val, op2_val);
		} else if (opline->opcode == ZEND_IS_SMALLER_OR_EQUAL) {
			cmp = llvm_ctx.builder.CreateICmpSLE(op1_val, op2_val);
		} else {
			ASSERT_NOT_REACHED();
		}
		if (opline->opcode != ZEND_CASE && OP1_OP_TYPE() == IS_VAR) {
			if (!zend_jit_free_operand(llvm_ctx, OP1_OP_TYPE(), orig_op1_addr, NULL, OP1_SSA_VAR(), OP1_INFO(), opline->lineno)) {
				return 0;
			}
		}
		if (OP2_OP_TYPE() == IS_VAR) {
			if (!zend_jit_free_operand(llvm_ctx, OP2_OP_TYPE(), orig_op2_addr, NULL, OP2_SSA_VAR(), OP2_INFO(), opline->lineno)) {
				return 0;
			}
		}
		if (bb_false && bb_true) {
			zend_jit_expected_br_ex(llvm_ctx, cmp,
				bb_true,
				bb_false,
				expected_branch);
		} else {
			Value *res = zend_jit_load_tmp_zval(llvm_ctx, RES_OP()->var);
			zend_jit_save_zval_type_info(llvm_ctx,
				res,
				RES_SSA_VAR(),
				RES_INFO(),
				llvm_ctx.builder.CreateAdd(
					llvm_ctx.builder.CreateZExtOrBitCast(
						cmp,
						Type::getInt32Ty(llvm_ctx.context)),
					llvm_ctx.builder.getInt32(IS_FALSE)));
		}
		if (n > 1) {
			llvm_ctx.builder.CreateBr(bb_finish);
		}
	}

	if (bb_long_double) {
		llvm_ctx.builder.SetInsertPoint(bb_long_double);
		if (opline->opcode != ZEND_IS_IDENTICAL && opline->opcode != ZEND_IS_NOT_IDENTICAL) {
			bb_double_double_cvt = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			op1_val1 = llvm_ctx.builder.CreateSIToFP(
							zend_jit_load_lval_c(llvm_ctx, op1_addr, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO()),
							Type::getDoubleTy(llvm_ctx.context));
			op2_val1 = zend_jit_load_dval(llvm_ctx, op2_addr, OP2_SSA_VAR(), OP2_INFO()),
			llvm_ctx.builder.CreateBr(bb_double_double_cvt);
		} else {
			if (opline->opcode != ZEND_CASE && OP1_OP_TYPE() == IS_VAR) {
				if (!zend_jit_free_operand(llvm_ctx, OP1_OP_TYPE(), orig_op1_addr, NULL, OP1_SSA_VAR(), OP1_INFO(), opline->lineno)) {
					return 0;
				}
			}
			if (OP2_OP_TYPE() == IS_VAR) {
				if (!zend_jit_free_operand(llvm_ctx, OP2_OP_TYPE(), orig_op2_addr, NULL, OP2_SSA_VAR(), OP2_INFO(), opline->lineno)) { 
					return 0;
				}
			}
			if (opline->opcode == ZEND_IS_IDENTICAL) {
				if (bb_false && bb_true) {
					llvm_ctx.builder.CreateBr(bb_false);
				} else {
					Value *res = zend_jit_load_tmp_zval(llvm_ctx, RES_OP()->var);
					zend_jit_save_zval_type_info(llvm_ctx, res, RES_SSA_VAR(), RES_INFO(), llvm_ctx.builder.getInt32(IS_FALSE));
				}
			} else if (opline->opcode == ZEND_IS_NOT_IDENTICAL) {
				if (bb_false && bb_true) {
					llvm_ctx.builder.CreateBr(bb_true);
				} else {
					Value *res = zend_jit_load_tmp_zval(llvm_ctx, RES_OP()->var);
					zend_jit_save_zval_type_info(llvm_ctx, res, RES_SSA_VAR(), RES_INFO(), llvm_ctx.builder.getInt32(IS_TRUE));
				}
			} else {
				ASSERT_NOT_REACHED();
			}
			if (n > 1) {
				llvm_ctx.builder.CreateBr(bb_finish);
			}
		}
	}

	if (bb_double_long) {
		llvm_ctx.builder.SetInsertPoint(bb_double_long);
		if (opline->opcode != ZEND_IS_IDENTICAL && opline->opcode != ZEND_IS_NOT_IDENTICAL) {
			if (!bb_double_double_cvt) {
				bb_double_double_cvt = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			op1_val2 = zend_jit_load_dval(llvm_ctx, op1_addr, OP1_SSA_VAR(), OP1_INFO()),
			op2_val2 = llvm_ctx.builder.CreateSIToFP(
							zend_jit_load_lval_c(llvm_ctx, op2_addr, OP2_OP_TYPE(), OP2_OP(), OP2_SSA_VAR(), OP2_INFO()),
							Type::getDoubleTy(llvm_ctx.context));
			llvm_ctx.builder.CreateBr(bb_double_double_cvt);
		} else {
			if (opline->opcode != ZEND_CASE && OP1_OP_TYPE() == IS_VAR) {
				if (!zend_jit_free_operand(llvm_ctx, OP1_OP_TYPE(), orig_op1_addr, NULL, OP1_SSA_VAR(), OP1_INFO(), opline->lineno)) {
					return 0;
				}
			}
			if (OP2_OP_TYPE() == IS_VAR) {
				if (!zend_jit_free_operand(llvm_ctx, OP2_OP_TYPE(), orig_op2_addr, NULL, OP2_SSA_VAR(), OP2_INFO(), opline->lineno)) {
					return 0;
				}
			}
			if (opline->opcode == ZEND_IS_IDENTICAL) {
				if (bb_false && bb_true) {
					llvm_ctx.builder.CreateBr(bb_false);
				} else {
					Value *res = zend_jit_load_tmp_zval(llvm_ctx, RES_OP()->var);
					zend_jit_save_zval_type_info(llvm_ctx, res, RES_SSA_VAR(), RES_INFO(), llvm_ctx.builder.getInt32(IS_FALSE));
				}
			} else if (opline->opcode == ZEND_IS_NOT_IDENTICAL) {
				if (bb_false && bb_true) {
					llvm_ctx.builder.CreateBr(bb_true);
				} else {
					Value *res = zend_jit_load_tmp_zval(llvm_ctx, RES_OP()->var);
					zend_jit_save_zval_type_info(llvm_ctx, res, RES_SSA_VAR(), RES_INFO(), llvm_ctx.builder.getInt32(IS_TRUE));
				}
			} else {
				ASSERT_NOT_REACHED();
			}
			if (n > 1) {
				llvm_ctx.builder.CreateBr(bb_finish);
			}
		}
	}

	if (bb_double_double || bb_double_double_cvt) {
		if (bb_double_double) {
			llvm_ctx.builder.SetInsertPoint(bb_double_double);
			op1_val = zend_jit_load_dval(llvm_ctx, op1_addr, OP1_SSA_VAR(), OP1_INFO());
			op2_val = zend_jit_load_dval(llvm_ctx, op2_addr, OP2_SSA_VAR(), OP2_INFO());
			if (bb_double_double_cvt) {
				PHINode *phi;

				llvm_ctx.builder.CreateBr(bb_double_double_cvt);
				llvm_ctx.builder.SetInsertPoint(bb_double_double_cvt);

				// Create LLVM SSA Phi() functions
				phi = llvm_ctx.builder.CreatePHI(Type::getDoubleTy(llvm_ctx.context), 3);
				phi->addIncoming(op1_val, bb_double_double);
				if (bb_long_double) {
					phi->addIncoming(op1_val1, bb_long_double);
				}
				if (bb_double_long) {
					phi->addIncoming(op1_val2, bb_double_long);
				}
				op1_val = phi;

				phi = llvm_ctx.builder.CreatePHI(Type::getDoubleTy(llvm_ctx.context), 3);
				phi->addIncoming(op2_val, bb_double_double);
				if (bb_long_double) {
					phi->addIncoming(op2_val1, bb_long_double);
				}
				if (bb_double_long) {
					phi->addIncoming(op2_val2, bb_double_long);
				}
				op2_val = phi;
			}
		} else if (bb_long_double) {
			llvm_ctx.builder.SetInsertPoint(bb_double_double_cvt);
			op1_val = op1_val1;
			op2_val = op2_val1;
		} else if (bb_double_long) {
			llvm_ctx.builder.SetInsertPoint(bb_double_double_cvt);
			op1_val = op1_val2;
			op2_val = op2_val2;
		}
		if (opline->opcode == ZEND_IS_EQUAL || opline->opcode == ZEND_CASE || opline->opcode == ZEND_IS_IDENTICAL) {
			cmp = llvm_ctx.builder.CreateFCmpOEQ(op1_val, op2_val);
		} else if (opline->opcode == ZEND_IS_NOT_EQUAL || opline->opcode == ZEND_IS_NOT_IDENTICAL) {
			cmp = llvm_ctx.builder.CreateFCmpUNE(op1_val, op2_val);
		} else if (opline->opcode == ZEND_IS_SMALLER) {
			cmp = llvm_ctx.builder.CreateFCmpULT(op1_val, op2_val);
		} else if (opline->opcode == ZEND_IS_SMALLER_OR_EQUAL) {
			cmp = llvm_ctx.builder.CreateFCmpULE(op1_val, op2_val);
		} else {
			ASSERT_NOT_REACHED();
		}
		if (opline->opcode != ZEND_CASE && OP1_OP_TYPE() == IS_VAR) {
			if (!zend_jit_free_operand(llvm_ctx, OP1_OP_TYPE(), orig_op1_addr, NULL, OP1_SSA_VAR(), OP1_INFO(), opline->lineno)) {
			   	return 0;
			}
		}
		if (OP2_OP_TYPE() == IS_VAR) {
			if (!zend_jit_free_operand(llvm_ctx, OP2_OP_TYPE(), orig_op2_addr, NULL, OP2_SSA_VAR(), OP2_INFO(), opline->lineno)) {
				return 0;
			}
		}
		if (bb_false && bb_true) {
			zend_jit_expected_br_ex(llvm_ctx, cmp,
				bb_true,
				bb_false,
				expected_branch);
		} else {
			Value *res = zend_jit_load_tmp_zval(llvm_ctx, RES_OP()->var);
			zend_jit_save_zval_type_info(llvm_ctx,
				res,
				RES_SSA_VAR(), RES_INFO(),
				llvm_ctx.builder.CreateAdd(
					llvm_ctx.builder.CreateZExtOrBitCast(
						cmp,
						Type::getInt32Ty(llvm_ctx.context)),
					llvm_ctx.builder.getInt32(IS_FALSE)));
		}
		if (n > 1) {
			llvm_ctx.builder.CreateBr(bb_finish);
		}
	}

	if (bb_slow_path) {
		llvm_ctx.builder.SetInsertPoint(bb_slow_path);
		// Slow path
		if (opline->opcode == ZEND_IS_IDENTICAL &&
		    OP1_OP_TYPE() == IS_CONST &&
		    Z_TYPE_P(RT_CONSTANT(llvm_ctx.op_array, *OP1_OP())) == IS_NULL) {
			if (!OP2_MAY_BE(MAY_BE_NULL)) {
				cmp = llvm_ctx.builder.getInt1(0);
			} else if (!OP2_MAY_BE(MAY_BE_ANY-MAY_BE_NULL)) {
				cmp = llvm_ctx.builder.getInt1(1);
			} else {
				Value *op2_type = zend_jit_load_type_c(llvm_ctx, op2_addr, OP2_OP_TYPE(), OP2_OP(), OP2_SSA_VAR(), OP2_INFO());
				cmp = llvm_ctx.builder.CreateICmpEQ(
						op2_type,
						llvm_ctx.builder.getInt8(IS_NULL));
			}
			if (!zend_jit_free_operand(llvm_ctx, OP2_OP_TYPE(), orig_op2_addr, NULL, OP2_SSA_VAR(), OP2_INFO(), opline->lineno)) {
				return 0;
			}
		} else if (opline->opcode == ZEND_IS_NOT_IDENTICAL &&
		    OP1_OP_TYPE() == IS_CONST &&
		    Z_TYPE_P(RT_CONSTANT(llvm_ctx.op_array, *OP1_OP())) == IS_NULL) {
			if (!OP2_MAY_BE(MAY_BE_NULL)) {
				cmp = llvm_ctx.builder.getInt1(1);
			} else if (!OP2_MAY_BE(MAY_BE_ANY-MAY_BE_NULL)) {
				cmp = llvm_ctx.builder.getInt1(0);
			} else {
				Value *op2_type = zend_jit_load_type_c(llvm_ctx, op2_addr, OP2_OP_TYPE(), OP2_OP(), OP2_SSA_VAR(), OP2_INFO());
				cmp = llvm_ctx.builder.CreateICmpNE(
						op2_type,
						llvm_ctx.builder.getInt8(IS_NULL));
			}
			if (!zend_jit_free_operand(llvm_ctx, OP2_OP_TYPE(), orig_op2_addr, NULL, OP2_SSA_VAR(), OP2_INFO(), opline->lineno)) {
				return 0;
			}
		} else if (opline->opcode == ZEND_IS_IDENTICAL &&
		    OP2_OP_TYPE() == IS_CONST &&
		    Z_TYPE_P(RT_CONSTANT(llvm_ctx.op_array, *OP2_OP())) == IS_NULL) {
			if (!OP1_MAY_BE(MAY_BE_NULL)) {
				cmp = llvm_ctx.builder.getInt1(0);
			} else if (!OP1_MAY_BE(MAY_BE_ANY-MAY_BE_NULL)) {
				cmp = llvm_ctx.builder.getInt1(1);
			} else {
				Value *op1_type = zend_jit_load_type_c(llvm_ctx, op1_addr, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO());
				cmp = llvm_ctx.builder.CreateICmpEQ(
						op1_type,
						llvm_ctx.builder.getInt8(IS_NULL));
			}
			if (!zend_jit_free_operand(llvm_ctx, OP1_OP_TYPE(), orig_op1_addr, NULL, OP1_SSA_VAR(), OP1_INFO(), opline->lineno)) {
				return 0;
			}
		} else if (opline->opcode == ZEND_IS_NOT_IDENTICAL &&
		    OP2_OP_TYPE() == IS_CONST &&
		    Z_TYPE_P(RT_CONSTANT(llvm_ctx.op_array, *OP2_OP())) == IS_NULL) {
			if (!OP1_MAY_BE(MAY_BE_NULL)) {
				cmp = llvm_ctx.builder.getInt1(1);
			} else if (!OP1_MAY_BE(MAY_BE_ANY-MAY_BE_NULL)) {
				cmp = llvm_ctx.builder.getInt1(0);
			} else {
				Value *op1_type = zend_jit_load_type_c(llvm_ctx, op1_addr, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO());
				cmp = llvm_ctx.builder.CreateICmpNE(
						op1_type,
						llvm_ctx.builder.getInt8(IS_NULL));
			}
			if (!zend_jit_free_operand(llvm_ctx, OP1_OP_TYPE(), orig_op1_addr, NULL, OP1_SSA_VAR(), OP1_INFO(), opline->lineno)) {
			   	return 0;
			}
		} else if (opline->opcode == ZEND_IS_IDENTICAL &&
		    OP1_OP_TYPE() == IS_CONST &&
		    Z_TYPE_P(RT_CONSTANT(llvm_ctx.op_array, *OP1_OP())) == IS_FALSE) {
			if (!OP2_MAY_BE(MAY_BE_FALSE)) {
				cmp = llvm_ctx.builder.getInt1(0);
			} else if (!OP2_MAY_BE(MAY_BE_ANY-MAY_BE_FALSE)) {
				cmp = llvm_ctx.builder.getInt1(1);
			} else {
				Value *op2_type = zend_jit_load_type_c(llvm_ctx, op2_addr, OP2_OP_TYPE(), OP2_OP(), OP2_SSA_VAR(), OP2_INFO());
				cmp = llvm_ctx.builder.CreateICmpEQ(
							op2_type,
							llvm_ctx.builder.getInt8(IS_FALSE));
			}
			if (!zend_jit_free_operand(llvm_ctx, OP2_OP_TYPE(), orig_op2_addr, NULL, OP2_SSA_VAR(), OP2_INFO(), opline->lineno)) {
				return 0;
			}
		} else if (opline->opcode == ZEND_IS_IDENTICAL &&
		    OP1_OP_TYPE() == IS_CONST &&
		    Z_TYPE_P(RT_CONSTANT(llvm_ctx.op_array, *OP1_OP())) == IS_TRUE) {
			if (!OP2_MAY_BE(MAY_BE_TRUE)) {
				cmp = llvm_ctx.builder.getInt1(0);
			} else if (!OP2_MAY_BE(MAY_BE_ANY-MAY_BE_TRUE)) {
				cmp = llvm_ctx.builder.getInt1(1);
			} else {
				Value *op2_type = zend_jit_load_type_c(llvm_ctx, op2_addr, OP2_OP_TYPE(), OP2_OP(), OP2_SSA_VAR(), OP2_INFO());
				cmp = llvm_ctx.builder.CreateICmpEQ(
							op2_type,
							llvm_ctx.builder.getInt8(IS_TRUE));
			}
			if (!zend_jit_free_operand(llvm_ctx, OP2_OP_TYPE(), orig_op2_addr, NULL, OP2_SSA_VAR(), OP2_INFO(), opline->lineno)) {
				return 0;
			}
		} else if (opline->opcode == ZEND_IS_NOT_IDENTICAL &&
		    OP1_OP_TYPE() == IS_CONST &&
		    Z_TYPE_P(RT_CONSTANT(llvm_ctx.op_array, *OP1_OP())) == IS_FALSE) {
			if (!OP2_MAY_BE(MAY_BE_FALSE)) {
				cmp = llvm_ctx.builder.getInt1(1);
			} else if (!OP2_MAY_BE(MAY_BE_ANY-MAY_BE_FALSE)) {
				cmp = llvm_ctx.builder.getInt1(0);
			} else {
				Value *op2_type = zend_jit_load_type_c(llvm_ctx, op2_addr, OP2_OP_TYPE(), OP2_OP(), OP2_SSA_VAR(), OP2_INFO());
				cmp = llvm_ctx.builder.CreateICmpNE(
							op2_type,
							llvm_ctx.builder.getInt8(IS_FALSE));
			}
			if (!zend_jit_free_operand(llvm_ctx, OP2_OP_TYPE(), orig_op2_addr, NULL, OP2_SSA_VAR(), OP2_INFO(), opline->lineno)) {
				return 0;
			}
		} else if (opline->opcode == ZEND_IS_NOT_IDENTICAL &&
		    OP1_OP_TYPE() == IS_CONST &&
		    Z_TYPE_P(RT_CONSTANT(llvm_ctx.op_array, *OP1_OP())) == IS_TRUE) {
			if (!OP2_MAY_BE(MAY_BE_TRUE)) {
				cmp = llvm_ctx.builder.getInt1(1);
			} else if (!OP2_MAY_BE(MAY_BE_ANY-MAY_BE_TRUE)) {
				cmp = llvm_ctx.builder.getInt1(0);
			} else {
				Value *op2_type = zend_jit_load_type_c(llvm_ctx, op2_addr, OP2_OP_TYPE(), OP2_OP(), OP2_SSA_VAR(), OP2_INFO());
				cmp = llvm_ctx.builder.CreateICmpNE(
							op2_type,
							llvm_ctx.builder.getInt8(IS_TRUE));
			}
			if (!zend_jit_free_operand(llvm_ctx, OP2_OP_TYPE(), orig_op2_addr, NULL, OP2_SSA_VAR(), OP2_INFO(), opline->lineno)) {
				return 0;
			}
		} else if (opline->opcode == ZEND_IS_IDENTICAL &&
		    OP2_OP_TYPE() == IS_CONST &&
		    Z_TYPE_P(RT_CONSTANT(llvm_ctx.op_array, *OP2_OP())) == IS_FALSE) {
			if (!OP1_MAY_BE(MAY_BE_FALSE)) {
				cmp = llvm_ctx.builder.getInt1(0);
			} else if (!OP1_MAY_BE(MAY_BE_ANY-MAY_BE_FALSE)) {
				cmp = llvm_ctx.builder.getInt1(1);
			} else {
				Value *op1_type = zend_jit_load_type_c(llvm_ctx, op1_addr, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO());
				cmp = llvm_ctx.builder.CreateICmpEQ(
							op1_type,
							llvm_ctx.builder.getInt8(IS_FALSE));
			}
			if (!zend_jit_free_operand(llvm_ctx, OP1_OP_TYPE(), orig_op1_addr, NULL, OP1_SSA_VAR(), OP1_INFO(), opline->lineno)) {
			   	return 0;
			}
		} else if (opline->opcode == ZEND_IS_IDENTICAL &&
		    OP2_OP_TYPE() == IS_CONST &&
		    Z_TYPE_P(RT_CONSTANT(llvm_ctx.op_array, *OP2_OP())) == IS_TRUE) {
			if (!OP1_MAY_BE(MAY_BE_TRUE)) {
				cmp = llvm_ctx.builder.getInt1(0);
			} else if (!OP1_MAY_BE(MAY_BE_ANY-MAY_BE_TRUE)) {
				cmp = llvm_ctx.builder.getInt1(1);
			} else {
				Value *op1_type = zend_jit_load_type_c(llvm_ctx, op1_addr, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO());
				cmp = llvm_ctx.builder.CreateICmpEQ(
							op1_type,
							llvm_ctx.builder.getInt8(IS_TRUE));
			}
			if (!zend_jit_free_operand(llvm_ctx, OP1_OP_TYPE(), orig_op1_addr, NULL, OP1_SSA_VAR(), OP1_INFO(), opline->lineno)) {
			   	return 0;
			}
		} else if (opline->opcode == ZEND_IS_NOT_IDENTICAL &&
		    OP2_OP_TYPE() == IS_CONST &&
		    Z_TYPE_P(RT_CONSTANT(llvm_ctx.op_array, *OP2_OP())) == IS_FALSE) {
			if (!OP1_MAY_BE(MAY_BE_FALSE)) {
				cmp = llvm_ctx.builder.getInt1(1);
			} else if (!OP1_MAY_BE(MAY_BE_ANY-MAY_BE_FALSE)) {
				cmp = llvm_ctx.builder.getInt1(0);
			} else {
				Value *op1_type = zend_jit_load_type_c(llvm_ctx, op1_addr, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO());
				cmp = llvm_ctx.builder.CreateICmpNE(
							op1_type,
							llvm_ctx.builder.getInt8(IS_FALSE));
			}
			if (!zend_jit_free_operand(llvm_ctx, OP1_OP_TYPE(), orig_op1_addr, NULL, OP1_SSA_VAR(), OP1_INFO(), opline->lineno)) {
				return 0;
			}
		} else if (opline->opcode == ZEND_IS_NOT_IDENTICAL &&
		    OP2_OP_TYPE() == IS_CONST &&
		    Z_TYPE_P(RT_CONSTANT(llvm_ctx.op_array, *OP2_OP())) == IS_TRUE) {
			if (!OP1_MAY_BE(MAY_BE_TRUE)) {
				cmp = llvm_ctx.builder.getInt1(1);
			} else if (!OP1_MAY_BE(MAY_BE_ANY-MAY_BE_TRUE)) {
				cmp = llvm_ctx.builder.getInt1(0);
			} else {
				Value *op1_type = zend_jit_load_type_c(llvm_ctx, op1_addr, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO());
				cmp = llvm_ctx.builder.CreateICmpNE(
							op1_type,
							llvm_ctx.builder.getInt8(IS_TRUE));
			}
			if (!zend_jit_free_operand(llvm_ctx, OP1_OP_TYPE(), orig_op1_addr, NULL, OP1_SSA_VAR(), OP1_INFO(), opline->lineno)) {
				return 0;
			}
		} else {
			void *helper;
			const char *name;
			if (opline->opcode == ZEND_IS_IDENTICAL || opline->opcode == ZEND_IS_NOT_IDENTICAL) {
				helper = (void*)is_identical_function;
				name = ZEND_JIT_SYM("is_identical_function");
			} else {
				helper = (void*)compare_function;
				name = ZEND_JIT_SYM("compare_function");
			}
			Function *_helper = zend_jit_get_helper(
				llvm_ctx,
				helper,
				name,
				ZEND_JIT_HELPER_FAST_CALL |
				ZEND_JIT_HELPER_ARG1_NOALIAS | ZEND_JIT_HELPER_ARG1_NOCAPTURE |
				ZEND_JIT_HELPER_ARG2_NOALIAS | ZEND_JIT_HELPER_ARG2_NOCAPTURE |
				ZEND_JIT_HELPER_ARG3_NOALIAS | ZEND_JIT_HELPER_ARG3_NOCAPTURE,
				Type::getInt32Ty(llvm_ctx.context),
				llvm_ctx.zval_ptr_type,
				llvm_ctx.zval_ptr_type,
				llvm_ctx.zval_ptr_type,
				NULL,
				NULL);

			if (OP1_MAY_BE(MAY_BE_IN_REG)) {
				op1_addr = zend_jit_reload_from_reg(llvm_ctx, OP1_SSA_VAR(), OP1_INFO());
			}

			if (OP2_MAY_BE(MAY_BE_IN_REG)) {
				op2_addr = zend_jit_reload_from_reg(llvm_ctx, OP2_SSA_VAR(), OP2_INFO());
			}

			if (!llvm_ctx.valid_opline && (OP1_MAY_BE(MAY_BE_OBJECT) || OP2_MAY_BE(MAY_BE_OBJECT))) {
				zend_jit_store_opline(llvm_ctx, opline, false);
			}

			CallInst *call = llvm_ctx.builder.CreateCall3(_helper,
				zend_jit_load_tmp_zval(llvm_ctx, RES_OP()->var),
				op1_addr,
				op2_addr);
			call->setCallingConv(CallingConv::X86_FastCall);

			if (opline->opcode != ZEND_CASE) {
				if (!zend_jit_free_operand(llvm_ctx, OP1_OP_TYPE(), orig_op1_addr, NULL, OP1_SSA_VAR(), OP1_INFO(), opline->lineno)) { 
				   	return 0;
				}
			}
			if (!zend_jit_free_operand(llvm_ctx, OP2_OP_TYPE(), orig_op2_addr, NULL, OP2_SSA_VAR(), OP2_INFO(), opline->lineno)) {
			   	return 0;
			}

			if (opline->opcode == ZEND_IS_IDENTICAL) {
				cmp = llvm_ctx.builder.CreateICmpEQ(
					zend_jit_load_type_info(
						llvm_ctx,
						zend_jit_load_tmp_zval(llvm_ctx, RES_OP()->var),
						-1, MAY_BE_ANY),
					llvm_ctx.builder.getInt32(IS_TRUE));
			} else if (opline->opcode == ZEND_IS_NOT_IDENTICAL) {
				cmp = llvm_ctx.builder.CreateICmpNE(
					zend_jit_load_type_info(
						llvm_ctx,
						zend_jit_load_tmp_zval(llvm_ctx, RES_OP()->var),
						-1, MAY_BE_ANY),
					llvm_ctx.builder.getInt32(IS_TRUE));
			} else if (opline->opcode == ZEND_IS_EQUAL || opline->opcode == ZEND_CASE) {
				cmp = llvm_ctx.builder.CreateICmpEQ(
					zend_jit_load_lval(
						llvm_ctx,
						zend_jit_load_tmp_zval(llvm_ctx, RES_OP()->var),
						-1, MAY_BE_LONG),
					LLVM_GET_LONG(0));
			} else if (opline->opcode == ZEND_IS_NOT_EQUAL || opline->opcode == ZEND_IS_NOT_IDENTICAL) {
				cmp = llvm_ctx.builder.CreateICmpNE(
					zend_jit_load_lval(
						llvm_ctx,
						zend_jit_load_tmp_zval(llvm_ctx, RES_OP()->var),
						-1, MAY_BE_LONG),
					LLVM_GET_LONG(0));
			} else if (opline->opcode == ZEND_IS_SMALLER) {
				cmp = llvm_ctx.builder.CreateICmpSLT(
					zend_jit_load_lval(
						llvm_ctx,
						zend_jit_load_tmp_zval(llvm_ctx, RES_OP()->var),
						-1, MAY_BE_LONG),
					LLVM_GET_LONG(0));
			} else if (opline->opcode == ZEND_IS_SMALLER_OR_EQUAL) {
				cmp = llvm_ctx.builder.CreateICmpSLE(
					zend_jit_load_lval(
						llvm_ctx,
						zend_jit_load_tmp_zval(llvm_ctx, RES_OP()->var),
						-1, MAY_BE_LONG),
					LLVM_GET_LONG(0));
			} else {
				ASSERT_NOT_REACHED();
			}
		}

		if (bb_false && bb_true) {
			zend_jit_expected_br_ex(llvm_ctx, cmp,
				bb_true,
				bb_false,
				expected_branch);
		} else {
			Value *res = zend_jit_load_tmp_zval(llvm_ctx, RES_OP()->var);
			zend_jit_save_zval_type_info(llvm_ctx,
				res,
				RES_SSA_VAR(), RES_INFO(),
				llvm_ctx.builder.CreateAdd(
					llvm_ctx.builder.CreateZExtOrBitCast(
						cmp,
						Type::getInt32Ty(llvm_ctx.context)),
					llvm_ctx.builder.getInt32(IS_FALSE)));
		}

		if (n > 1) {
			llvm_ctx.builder.CreateBr(bb_finish);
		}
	}

	if (n > 1) {
		llvm_ctx.builder.SetInsertPoint(bb_finish);
	}

	return 1;
}
/* }}} */

/* {{{ static int zend_jit_math */
static int zend_jit_math(zend_llvm_ctx    &llvm_ctx,
                         Value            *orig_op1_addr,
                         Value            *op1_addr,
						 zend_uchar        op1_op_type,
						 znode_op         *op1_op,
						 int               op1_ssa_var,
						 uint32_t          op1_info,
                         Value            *orig_op2_addr,
                         Value            *op2_addr,
						 zend_uchar        op2_op_type,
						 znode_op         *op2_op,
						 int               op2_ssa_var,
						 uint32_t          op2_info,
						 Value            *result_addr,
						 znode_op         *result_op,
						 int               result_ssa_var,
						 uint32_t          result_info,
						 zend_bool         same_cvs,
						 uint32_t          lineno,
						 zend_uchar        opcode,
						 zend_op          *opline,
						 int               check_exception)
{
	Value *op1_val = NULL;
	Value *op2_val = NULL;
	Value *op1_val0 = NULL;
	Value *op2_val0 = NULL;
	Value *op1_val1 = NULL;
	Value *op2_val1 = NULL;
	Value *op1_val2 = NULL;
	Value *op2_val2 = NULL;
	Value *res;
	BasicBlock *bb_long_long;
	BasicBlock *bb_long_double;
	BasicBlock *bb_double_long;
	BasicBlock *bb_double_double;
	BasicBlock *bb_slow_path;
	BasicBlock *bb_finish = NULL;
	BasicBlock *bb_overflow = NULL;
	BasicBlock *bb_double_double_cvt = NULL;
	int n;

	n = zend_jit_math_dispatch(llvm_ctx,
			op1_addr,
			op1_op_type,
			op1_op,
			op1_ssa_var,
			op1_info, 
			op2_addr,
			op2_op_type,
			op2_op,
			op2_ssa_var,
			op2_info, 
			same_cvs,
			&bb_long_long, 
			&bb_long_double,
			&bb_double_long,
			&bb_double_double,
			&bb_slow_path);

	if (n > 1) {
		bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	}

	if (bb_long_long) {
		llvm_ctx.builder.SetInsertPoint(bb_long_long);
		op1_val = zend_jit_load_lval_c(llvm_ctx, op1_addr, op1_op_type, op1_op, op1_ssa_var, op1_info);
		if (same_cvs) {
			op2_val = op1_val;
		} else {
			op2_val = zend_jit_load_lval_c(llvm_ctx, op2_addr, op2_op_type, op2_op, op2_ssa_var, op2_info);
		}

		if (opcode == ZEND_DIV) {
			long op1_min;
			long op2_min;
			long op2_max;

			if (op1_op_type == IS_CONST &&
			    Z_TYPE_P(RT_CONSTANT(llvm_ctx.op_array, *op1_op)) == IS_LONG) {
			    op1_min = Z_LVAL_P(RT_CONSTANT(llvm_ctx.op_array, *op1_op));
			} else if (op1_ssa_var >= 0 &&
			           JIT_DATA(llvm_ctx.op_array)->ssa_var_info &&
			           JIT_DATA(llvm_ctx.op_array)->ssa_var_info[op1_ssa_var].has_range) {
			    op1_min = JIT_DATA(llvm_ctx.op_array)->ssa_var_info[op1_ssa_var].range.min;
			} else {
				op1_min = LONG_MIN;
			}
			if (op2_op_type == IS_CONST &&
			    Z_TYPE_P(RT_CONSTANT(llvm_ctx.op_array, *op2_op)) == IS_LONG) {
			    op2_min = op2_max = Z_LVAL_P(RT_CONSTANT(llvm_ctx.op_array, *op2_op));
			} else if (op2_ssa_var >= 0 &&
			           JIT_DATA(llvm_ctx.op_array)->ssa_var_info &&
			           JIT_DATA(llvm_ctx.op_array)->ssa_var_info[op2_ssa_var].has_range) {
			    op2_min = JIT_DATA(llvm_ctx.op_array)->ssa_var_info[op2_ssa_var].range.min;
			    op2_max = JIT_DATA(llvm_ctx.op_array)->ssa_var_info[op2_ssa_var].range.max;
			} else {
				op2_min = LONG_MIN;
				op2_max = LONG_MAX;
			}

			BasicBlock *bb_fdiv = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			if (op2_min <= 0 && op2_max >= 0) {
				// Check for division by zero
				BasicBlock *bb_zero = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				BasicBlock *bb_non_zero = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				// JIT: if (Z_LVAL_P(op2) == 0) {
				zend_jit_unexpected_br(llvm_ctx,
					llvm_ctx.builder.CreateICmpEQ(
						op2_val,
						LLVM_GET_LONG(0)),
					bb_zero,
					bb_non_zero);
				llvm_ctx.builder.SetInsertPoint(bb_zero);
				// JIT: zend_error(E_WARNING, "Division by zero");
				zend_jit_error(llvm_ctx, opline, E_WARNING, "Division by zero");
				llvm_ctx.builder.CreateBr(bb_fdiv);
				llvm_ctx.builder.SetInsertPoint(bb_non_zero);
			}
			if (op1_min == LONG_MIN && op2_min <= -1 && op2_max >= -1) {
				/* Prevent overflow error/crash if op1==LONG_MIN */
				BasicBlock *bb_div = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				// JIT: if (Z_LVAL_P(op2) == 0) {
				zend_jit_unexpected_br(llvm_ctx,
					llvm_ctx.builder.CreateICmpEQ(
						op2_val,
						LLVM_GET_LONG(-1)),
					bb_follow,
					bb_div);
				llvm_ctx.builder.SetInsertPoint(bb_follow);
				bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				// JIT: if (Z_LVAL_P(op2) == 0) {
				zend_jit_unexpected_br(llvm_ctx,
					llvm_ctx.builder.CreateICmpEQ(
						op1_val,
						LLVM_GET_LONG(LONG_MIN)),
					bb_follow,
					bb_div);
				llvm_ctx.builder.SetInsertPoint(bb_follow);
				// JIT: ZVAL_DOUBLE(result, (double) LONG_MIN / -1.0);
				Value *result; 
				if (!result_addr) {
					result = zend_jit_load_tmp_zval(llvm_ctx, result_op->var);
				} else {
					result = result_addr;
				}
				zend_jit_save_zval_type_info(llvm_ctx, result, result_ssa_var, result_info, llvm_ctx.builder.getInt32(IS_DOUBLE));
				zend_jit_save_zval_dval(llvm_ctx, result, result_ssa_var, result_info, ConstantFP::get(Type::getDoubleTy(llvm_ctx.context), (double) LONG_MIN / -1.0));
				if (!bb_finish) {
					bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				}
				llvm_ctx.builder.CreateBr(bb_finish);
				llvm_ctx.builder.SetInsertPoint(bb_div);
			}
					
			BasicBlock *bb_sdiv = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			res = llvm_ctx.builder.CreateExactSDiv(op1_val, op2_val);
			zend_jit_unexpected_br(llvm_ctx,
					llvm_ctx.builder.CreateICmpEQ(
						llvm_ctx.builder.CreateSRem(op1_val, op2_val),
						LLVM_GET_LONG(0)),
					bb_sdiv,
					bb_fdiv);
			llvm_ctx.builder.SetInsertPoint(bb_fdiv);
			Value *fres = llvm_ctx.builder.CreateFDiv(
					llvm_ctx.builder.CreateSIToFP(
						op1_val,
						Type::getDoubleTy(llvm_ctx.context)),
					llvm_ctx.builder.CreateSIToFP(
						op2_val,
						Type::getDoubleTy(llvm_ctx.context)));
			Value *result; 
			if (!result_addr) {
				result = zend_jit_load_tmp_zval(llvm_ctx, result_op->var);
			} else {
				result = result_addr;
			}
			zend_jit_save_zval_type_info(llvm_ctx, result, result_ssa_var, result_info, llvm_ctx.builder.getInt32(IS_DOUBLE));
			zend_jit_save_zval_dval(llvm_ctx, result, result_ssa_var, result_info, fres);
			if (!bb_finish) {
				bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(bb_finish);
			llvm_ctx.builder.SetInsertPoint(bb_sdiv);
		} else if (result_info & (MAY_BE_DOUBLE)) {
		    // May overflow
		    if (!bb_finish) {
				bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		    }
            Intrinsic::ID id;
			if (opcode == ZEND_ADD) {
				id = Intrinsic::sadd_with_overflow;
			} else if (opcode == ZEND_SUB) {
				id = Intrinsic::ssub_with_overflow;
			} else if (opcode == ZEND_MUL) {
				id = Intrinsic::smul_with_overflow;
			} else {
				ASSERT_NOT_REACHED();
			}
			Function *fun = Intrinsic::getDeclaration(llvm_ctx.module, id, ArrayRef<Type*>(LLVM_GET_LONG_TY(llvm_ctx.context)));
			Value *call = llvm_ctx.builder.CreateCall2(fun, op1_val, op2_val);
			BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_overflow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			zend_jit_unexpected_br(llvm_ctx,
			    llvm_ctx.builder.CreateExtractValue(call, 1),
				bb_overflow,
				bb_follow);
			llvm_ctx.builder.SetInsertPoint(bb_overflow);
			if (!bb_double_double_cvt) {
				bb_double_double_cvt = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			op1_val0 = llvm_ctx.builder.CreateSIToFP(
							op1_val,
							Type::getDoubleTy(llvm_ctx.context));
			if (same_cvs) {
				op2_val0 = op1_val0;
			} else {
				op2_val0 = llvm_ctx.builder.CreateSIToFP(
								op2_val,
								Type::getDoubleTy(llvm_ctx.context));
			}
			llvm_ctx.builder.CreateBr(bb_double_double_cvt);
			llvm_ctx.builder.SetInsertPoint(bb_follow);
			res = llvm_ctx.builder.CreateExtractValue(call, 0);
		} else {
			if (opcode == ZEND_ADD) {
				res = llvm_ctx.builder.CreateAdd(op1_val, op2_val);
			} else if (opcode == ZEND_SUB) {
				res = llvm_ctx.builder.CreateSub(op1_val, op2_val);
			} else if (opcode == ZEND_MUL) {
				res = llvm_ctx.builder.CreateMul(op1_val, op2_val);
			} else {
				ASSERT_NOT_REACHED();
			}
		}

		Value *result; 
		if (!result_addr) {
			result = zend_jit_load_tmp_zval(llvm_ctx, result_op->var);
		} else {
			result = result_addr;
		}
		zend_jit_save_zval_type_info(llvm_ctx, result, result_ssa_var, result_info, llvm_ctx.builder.getInt32(IS_LONG));
		zend_jit_save_zval_lval(llvm_ctx, result, result_ssa_var, result_info, res);

		if (bb_finish) {
			llvm_ctx.builder.CreateBr(bb_finish);
		}
	}

	if (bb_long_double) {
		llvm_ctx.builder.SetInsertPoint(bb_long_double);
		if (!bb_double_double_cvt) {
			bb_double_double_cvt = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		}
		op1_val1 = llvm_ctx.builder.CreateSIToFP(
						zend_jit_load_lval_c(llvm_ctx, op1_addr, op1_op_type, op1_op, op1_ssa_var, op1_info),
						Type::getDoubleTy(llvm_ctx.context));
		op2_val1 = zend_jit_load_dval(llvm_ctx, op2_addr, op2_ssa_var, op2_info),
		llvm_ctx.builder.CreateBr(bb_double_double_cvt);
	}

	if (bb_double_long) {
		llvm_ctx.builder.SetInsertPoint(bb_double_long);
		if (!bb_double_double_cvt) {
			bb_double_double_cvt = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		}
		op1_val2 = zend_jit_load_dval(llvm_ctx, op1_addr, op1_ssa_var, op1_info),
		op2_val2 = llvm_ctx.builder.CreateSIToFP(
						zend_jit_load_lval_c(llvm_ctx, op2_addr, op2_op_type, op2_op, op2_ssa_var, op2_info),
						Type::getDoubleTy(llvm_ctx.context));
		llvm_ctx.builder.CreateBr(bb_double_double_cvt);
	}

	if (bb_double_double || bb_double_double_cvt) {
		if (bb_double_double) {
			llvm_ctx.builder.SetInsertPoint(bb_double_double);
			op1_val = zend_jit_load_dval(llvm_ctx, op1_addr, op1_ssa_var, op1_info);
			if (same_cvs) {
				op2_val = op1_val;
			} else {
				op2_val = zend_jit_load_dval(llvm_ctx, op2_addr, op2_ssa_var, op2_info);
			}
			if (bb_double_double_cvt) {
				PHINode *phi;

				llvm_ctx.builder.CreateBr(bb_double_double_cvt);
				llvm_ctx.builder.SetInsertPoint(bb_double_double_cvt);

				// Create LLVM SSA Phi() functions
				phi = llvm_ctx.builder.CreatePHI(Type::getDoubleTy(llvm_ctx.context), 4);
				phi->addIncoming(op1_val, bb_double_double);
				if (bb_overflow) {
					phi->addIncoming(op1_val0, bb_overflow);
				}
				if (bb_long_double) {
					phi->addIncoming(op1_val1, bb_long_double);
				}
				if (bb_double_long) {
					phi->addIncoming(op1_val2, bb_double_long);
				}
				op1_val = phi;

				if (same_cvs) {
					op2_val = op1_val;
				} else {
					phi = llvm_ctx.builder.CreatePHI(Type::getDoubleTy(llvm_ctx.context), 3);
					phi->addIncoming(op2_val, bb_double_double);
					if (bb_overflow) {
						phi->addIncoming(op2_val0, bb_overflow);
					}
					if (bb_long_double) {
						phi->addIncoming(op2_val1, bb_long_double);
					}
					if (bb_double_long) {
						phi->addIncoming(op2_val2, bb_double_long);
					}
					op2_val = phi;
	        	}
			}
		} else if (bb_overflow) {
			llvm_ctx.builder.SetInsertPoint(bb_double_double_cvt);
			if (bb_long_double) {
				PHINode *phi;
				phi = llvm_ctx.builder.CreatePHI(Type::getDoubleTy(llvm_ctx.context), 2);
				phi->addIncoming(op1_val0, bb_overflow);
				phi->addIncoming(op1_val1, bb_long_double);
				op1_val = phi;
				phi = llvm_ctx.builder.CreatePHI(Type::getDoubleTy(llvm_ctx.context), 2);
				phi->addIncoming(op2_val0, bb_overflow);
				phi->addIncoming(op2_val1, bb_long_double);
				op2_val = phi;
			} else if (bb_double_long) {
				PHINode *phi;
				phi = llvm_ctx.builder.CreatePHI(Type::getDoubleTy(llvm_ctx.context), 2);
				phi->addIncoming(op1_val0, bb_overflow);
				phi->addIncoming(op1_val2, bb_double_long);
				op1_val = phi;
				phi = llvm_ctx.builder.CreatePHI(Type::getDoubleTy(llvm_ctx.context), 2);
				phi->addIncoming(op2_val0, bb_overflow);
				phi->addIncoming(op2_val2, bb_double_long);
				op2_val = phi;
			} else {
				op1_val = op1_val0;
				op2_val = op2_val0;
			}
		} else if (bb_long_double) {
			llvm_ctx.builder.SetInsertPoint(bb_double_double_cvt);
			op1_val = op1_val1;
			op2_val = op2_val1;
		} else if (bb_double_long) {
			llvm_ctx.builder.SetInsertPoint(bb_double_double_cvt);
			op1_val = op1_val2;
			op2_val = op2_val2;
		}

		if (opcode == ZEND_ADD) {
			res = llvm_ctx.builder.CreateFAdd(op1_val, op2_val);
		} else if (opcode == ZEND_SUB) {
			res = llvm_ctx.builder.CreateFSub(op1_val, op2_val);
		} else if (opcode == ZEND_MUL) {
			res = llvm_ctx.builder.CreateFMul(op1_val, op2_val);
		} else if (opcode == ZEND_DIV) {
			long op2_min;
			long op2_max;

			if (op2_op_type == IS_CONST &&
			    Z_TYPE_P(RT_CONSTANT(llvm_ctx.op_array, *op2_op)) == IS_LONG) {
			    op2_min = op2_max = Z_LVAL_P(RT_CONSTANT(llvm_ctx.op_array, *op2_op));
			} else if (op2_op_type == IS_CONST &&
			    Z_TYPE_P(RT_CONSTANT(llvm_ctx.op_array, *op2_op)) == IS_DOUBLE) {
				if (Z_DVAL_P(RT_CONSTANT(llvm_ctx.op_array, *op2_op)) == 0.0) {
				    op2_min = op2_max = 0;
				} else {
				    op2_min = op2_max = 1;
				}
			} else if (op2_ssa_var >= 0 &&
			           JIT_DATA(llvm_ctx.op_array)->ssa_var_info &&
			           JIT_DATA(llvm_ctx.op_array)->ssa_var_info[op2_ssa_var].has_range) {
			    op2_min = JIT_DATA(llvm_ctx.op_array)->ssa_var_info[op2_ssa_var].range.min;
			    op2_max = JIT_DATA(llvm_ctx.op_array)->ssa_var_info[op2_ssa_var].range.max;
			} else {
				op2_min = LONG_MIN;
				op2_max = LONG_MAX;
			}
			if (op2_min <= 0 && op2_max >= 0) {
				// Check for division by zero
				BasicBlock *bb_zero = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				BasicBlock *bb_non_zero = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				// JIT: if (Z_DVAL_P(op2) == 0) {
				zend_jit_unexpected_br(llvm_ctx,
					llvm_ctx.builder.CreateFCmpOEQ(
						op2_val,
						ConstantFP::get(Type::getDoubleTy(llvm_ctx.context), 0.0)),
					bb_zero,
					bb_non_zero);
				llvm_ctx.builder.SetInsertPoint(bb_zero);
				// JIT: zend_error(E_WARNING, "Division by zero");
				zend_jit_error(llvm_ctx, opline, E_WARNING, "Division by zero");
				llvm_ctx.builder.CreateBr(bb_non_zero);
				llvm_ctx.builder.SetInsertPoint(bb_non_zero);
			}
			res = llvm_ctx.builder.CreateFDiv(op1_val, op2_val);
		} else {
			ASSERT_NOT_REACHED();
		}

		Value *result; 
		if (!result_addr) {
			result = zend_jit_load_tmp_zval(llvm_ctx, result_op->var);
		} else {
			result = result_addr;
		}
		zend_jit_save_zval_type_info(llvm_ctx, result, result_ssa_var, result_info, llvm_ctx.builder.getInt32(IS_DOUBLE));
		zend_jit_save_zval_dval(llvm_ctx, result, result_ssa_var, result_info, res);

		if (bb_finish) {
			llvm_ctx.builder.CreateBr(bb_finish);
		}
	}

	if (bb_slow_path) {
		llvm_ctx.builder.SetInsertPoint(bb_slow_path);
		// Slow path
		void *helper;
		const char *name;
		if (opcode == ZEND_ADD) {
			helper = (void*)add_function;
			name = ZEND_JIT_SYM("add_function");
		} else if (opcode == ZEND_SUB) {
			helper = (void*)sub_function;
			name = ZEND_JIT_SYM("sub_function");
		} else if (opcode == ZEND_MUL) {
			helper = (void*)mul_function;
			name = ZEND_JIT_SYM("mul_function");
		} else if (opcode == ZEND_DIV) {
			helper = (void*)div_function;
			name = ZEND_JIT_SYM("div_function");
		} else {
			ASSERT_NOT_REACHED();
		}

		Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			helper,
			name,
			ZEND_JIT_HELPER_FAST_CALL | 
			ZEND_JIT_HELPER_ARG1_NOALIAS | ZEND_JIT_HELPER_ARG1_NOCAPTURE |
			ZEND_JIT_HELPER_ARG2_NOALIAS | ZEND_JIT_HELPER_ARG2_NOCAPTURE |
			ZEND_JIT_HELPER_ARG3_NOALIAS | ZEND_JIT_HELPER_ARG3_NOCAPTURE,
			Type::getInt32Ty(llvm_ctx.context),
			llvm_ctx.zval_ptr_type,
			llvm_ctx.zval_ptr_type,
			llvm_ctx.zval_ptr_type,
			NULL,
			NULL);

		//JIT: SAVE_OPLINE();
		if (!llvm_ctx.valid_opline) {
			JIT_CHECK(zend_jit_store_opline(llvm_ctx, opline, false));
		}
		if (op1_info & (MAY_BE_IN_REG)) {
			op1_addr = zend_jit_reload_from_reg(llvm_ctx, op1_ssa_var, op1_info);
		}
		if (op2_info & (MAY_BE_IN_REG)) {
			op2_addr = zend_jit_reload_from_reg(llvm_ctx, op2_ssa_var, op2_info);
		}
		CallInst *call = llvm_ctx.builder.CreateCall3(_helper,
			result_addr ? result_addr : zend_jit_load_tmp_zval(llvm_ctx, result_op->var),
			op1_addr,
			op2_addr);
		call->setCallingConv(CallingConv::X86_FastCall);		

		if (result_info & (MAY_BE_IN_REG)) {
			zend_jit_reload_to_reg(llvm_ctx,
				result_addr ? result_addr : zend_jit_load_tmp_zval(llvm_ctx, result_op->var),
				result_ssa_var, result_info);
		}

		if (op1_addr != result_addr) {
			if (orig_op1_addr && !zend_jit_free_operand(llvm_ctx, op1_op_type, orig_op1_addr, NULL, op1_ssa_var, op1_info, lineno)) {
				return 0;
			}
		}
		if (orig_op2_addr && !zend_jit_free_operand(llvm_ctx, op2_op_type, orig_op2_addr, NULL, op2_ssa_var, op2_info, lineno)) {
			return 0;
		}

		if (check_exception) {
			//JIT: CHECK_EXCEPTION
			if ((op1_info & (MAY_BE_ARRAY|MAY_BE_OBJECT|MAY_BE_RESOURCE)) ||
			    (op2_info & (MAY_BE_ARRAY|MAY_BE_OBJECT|MAY_BE_RESOURCE))) {
				JIT_CHECK(zend_jit_check_exception(llvm_ctx, opline));
			}
		}

		if (bb_finish) {
			llvm_ctx.builder.CreateBr(bb_finish);
		}
	}

	if (bb_finish) {
		llvm_ctx.builder.SetInsertPoint(bb_finish);
	}

	return 1;
}
/* }}} */

/* {{{ static int zend_jit_long_math */
static int zend_jit_long_math(zend_llvm_ctx    &llvm_ctx,
                              Value            *orig_op1_addr,
                              Value            *op1_addr,
						      zend_uchar        op1_op_type,
						      znode_op         *op1_op,
						      int               op1_ssa_var,
						      uint32_t          op1_info,
                              Value            *orig_op2_addr,
                              Value            *op2_addr,
						      zend_uchar        op2_op_type,
						      znode_op         *op2_op,
						      int               op2_ssa_var,
						      uint32_t          op2_info,
						      Value            *result_addr,
						      znode_op         *result_op,
						      int               result_ssa_var,
						      uint32_t          result_info,
						      zend_bool         same_cvs,
						      uint32_t          lineno,
						      uint32_t          opcode,
						      zend_op          *opline)
{
	Value *op1_val = NULL;
	Value *op2_val = NULL;
	Value *res;
	BasicBlock *bb_slow_path = NULL;
	BasicBlock *bb_finish = NULL;
	int n;

	if ((op1_info & MAY_BE_LONG) & (op2_info & MAY_BE_LONG)) {
		if (op1_info & (MAY_BE_ANY-MAY_BE_LONG)) {
			BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_slow_path = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			Value *op1_type = zend_jit_load_type_c(llvm_ctx, op1_addr, op1_op_type, op1_op, op1_ssa_var, op1_info);
			zend_jit_expected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(
					op1_type,
					llvm_ctx.builder.getInt8(IS_LONG)),
				bb_follow,
				bb_slow_path);
			llvm_ctx.builder.SetInsertPoint(bb_follow);
		}
		if (op2_info & (MAY_BE_ANY-MAY_BE_LONG)) {
			BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			if (!bb_slow_path) {
				bb_slow_path = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			if (!bb_finish) {
				bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			Value *op2_type = zend_jit_load_type_c(llvm_ctx, op2_addr, op2_op_type, op2_op, op2_ssa_var, op2_info);
			zend_jit_expected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(
					op2_type,
					llvm_ctx.builder.getInt8(IS_LONG)),
				bb_follow,
				bb_slow_path);
			llvm_ctx.builder.SetInsertPoint(bb_follow);
		}
		op1_val = zend_jit_load_lval_c(llvm_ctx, op1_addr, op1_op_type, op1_op, op1_ssa_var, op1_info);
		if (same_cvs) {
			op2_val = op1_val;
		} else {
			op2_val = zend_jit_load_lval_c(llvm_ctx, op2_addr, op2_op_type, op2_op, op2_ssa_var, op2_info);
		}
		if (opcode == ZEND_MOD) {
			long op1_min;
			long op2_min;
			long op2_max;

			if (op1_op_type == IS_CONST &&
			    Z_TYPE_P(RT_CONSTANT(llvm_ctx.op_array, *op1_op)) == IS_LONG) {
			    op1_min = Z_LVAL_P(RT_CONSTANT(llvm_ctx.op_array, *op1_op));
			} else if (op1_ssa_var >= 0 &&
			           JIT_DATA(llvm_ctx.op_array)->ssa_var_info &&
			           JIT_DATA(llvm_ctx.op_array)->ssa_var_info[op1_ssa_var].has_range) {
			    op1_min = JIT_DATA(llvm_ctx.op_array)->ssa_var_info[op1_ssa_var].range.min;
			} else {
				op1_min = LONG_MIN;
			}
			if (op2_op_type == IS_CONST &&
			    Z_TYPE_P(RT_CONSTANT(llvm_ctx.op_array, *op2_op)) == IS_LONG) {
			    op2_min = op2_max = Z_LVAL_P(RT_CONSTANT(llvm_ctx.op_array, *op2_op));
			} else if (op2_ssa_var >= 0 &&
			           JIT_DATA(llvm_ctx.op_array)->ssa_var_info &&
			           JIT_DATA(llvm_ctx.op_array)->ssa_var_info[op2_ssa_var].has_range) {
			    op2_min = JIT_DATA(llvm_ctx.op_array)->ssa_var_info[op2_ssa_var].range.min;
			    op2_max = JIT_DATA(llvm_ctx.op_array)->ssa_var_info[op2_ssa_var].range.max;
			} else {
				op2_min = LONG_MIN;
				op2_max = LONG_MAX;
			}

			if (op2_min <= 0 && op2_max >= 0) {
				// Check for division by zero
				BasicBlock *bb_zero = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				BasicBlock *bb_non_zero = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				// JIT: if (Z_LVAL_P(op2) == 0) {
				zend_jit_unexpected_br(llvm_ctx,
					llvm_ctx.builder.CreateICmpEQ(
						op2_val,
						LLVM_GET_LONG(0)),
					bb_zero,
					bb_non_zero);
				llvm_ctx.builder.SetInsertPoint(bb_zero);
				// JIT: zend_throw_exception_ex(zend_ce_division_by_zero_error, 0, "Modulo by zero");
				zend_jit_throw_error(llvm_ctx, opline,
						zend_ce_division_by_zero_error, "Modulo by zero");
				llvm_ctx.builder.CreateBr(zend_jit_find_exception_bb(llvm_ctx, opline));
				llvm_ctx.builder.SetInsertPoint(bb_non_zero);
			}
			if (op1_min == LONG_MIN && op2_min <= -1 && op2_max >= -1) {
				/* Prevent overflow error/crash if op1==LONG_MIN */
				BasicBlock *bb_mod = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				// JIT: if (Z_LVAL_P(op2) == 0) {
				zend_jit_unexpected_br(llvm_ctx,
					llvm_ctx.builder.CreateICmpEQ(
						op2_val,
						LLVM_GET_LONG(-1)),
					bb_follow,
					bb_mod);
				llvm_ctx.builder.SetInsertPoint(bb_follow);
				// JIT: ZVAL_LONG(result, 0);
				Value *result; 
				if (!result_addr) {
					result = zend_jit_load_tmp_zval(llvm_ctx, result_op->var);
				} else {
					result = result_addr;
				}
				zend_jit_save_zval_type_info(llvm_ctx, result, result_ssa_var, result_info, llvm_ctx.builder.getInt32(IS_LONG));
				zend_jit_save_zval_lval(llvm_ctx, result, result_ssa_var, result_info, LLVM_GET_LONG(0));
				if (op1_op_type == IS_VAR && op1_addr != result_addr) {
					if (!zend_jit_free_operand(llvm_ctx, op1_op_type, orig_op1_addr, NULL, op1_ssa_var, op1_info, lineno)) {
						return 0;
					}
				}
				if (op2_op_type == IS_VAR) {
					if (!zend_jit_free_operand(llvm_ctx, op2_op_type, orig_op2_addr, NULL, op2_ssa_var, op2_info, lineno)) {
						return 0;
					}
				}
				if (!bb_finish) {
					bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				}
				llvm_ctx.builder.CreateBr(bb_finish);
				llvm_ctx.builder.SetInsertPoint(bb_mod);
			}
			res = llvm_ctx.builder.CreateSRem(op1_val, op2_val);
		} else if (opcode == ZEND_SL) {
			res = llvm_ctx.builder.CreateShl(op1_val, op2_val);
		} else if (opcode == ZEND_SR) {
			res = llvm_ctx.builder.CreateAShr(op1_val, op2_val);
		} else if (opcode == ZEND_BW_AND) {
			res = llvm_ctx.builder.CreateAnd(op1_val, op2_val);
		} else if (opcode == ZEND_BW_OR) {
			res = llvm_ctx.builder.CreateOr(op1_val, op2_val);
		} else if (opcode == ZEND_BW_XOR) {
			res = llvm_ctx.builder.CreateXor(op1_val, op2_val);
		} else {
			ASSERT_NOT_REACHED();
		}
		Value *result; 
		if (!result_addr) {
			result = zend_jit_load_tmp_zval(llvm_ctx, result_op->var);
		} else {
			result = result_addr;
		}
		zend_jit_save_zval_type_info(llvm_ctx, result, result_ssa_var, result_info, llvm_ctx.builder.getInt32(IS_LONG));
		zend_jit_save_zval_lval(llvm_ctx, result, result_ssa_var, result_info, res);

		if (op1_op_type == IS_VAR && op1_addr != result_addr) {
			if (!zend_jit_free_operand(llvm_ctx, op1_op_type, orig_op1_addr, NULL, op1_ssa_var, op1_info, lineno)) {
				return 0;
			}
		}
		if (op2_op_type == IS_VAR) {
			if (!zend_jit_free_operand(llvm_ctx, op2_op_type, orig_op2_addr, NULL, op2_ssa_var, op2_info, lineno)) {
				return 0;
			}
		}
		if (bb_finish) {
			llvm_ctx.builder.CreateBr(bb_finish);
		}
	}
	if ((op1_info & (MAY_BE_ANY-MAY_BE_LONG)) ||
	    (op2_info & (MAY_BE_ANY-MAY_BE_LONG))) {
		if (bb_slow_path) {
			llvm_ctx.builder.SetInsertPoint(bb_slow_path);
		}

		// Slow path
		void *helper;
		const char *name;
		if (opcode == ZEND_MOD) {
			helper = (void*)mod_function;
			name = ZEND_JIT_SYM("mod_function");
		} else if (opcode == ZEND_SL) {
			helper = (void*)shift_left_function;
			name = ZEND_JIT_SYM("shift_left_function");
		} else if (opcode == ZEND_SR) {
			helper = (void*)shift_right_function;
			name = ZEND_JIT_SYM("shift_right_function");
		} else if (opcode == ZEND_BW_AND) {
			helper = (void*)bitwise_and_function;
			name = ZEND_JIT_SYM("bitwise_and_function");
		} else if (opcode == ZEND_BW_OR) {
			helper = (void*)bitwise_or_function;
			name = ZEND_JIT_SYM("bitwise_or_function");
		} else if (opcode == ZEND_BW_XOR) {
			helper = (void*)bitwise_xor_function;
			name = ZEND_JIT_SYM("bitwise_xor_function");
		} else {
			ASSERT_NOT_REACHED();
		}
		Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			helper,
			name,
			ZEND_JIT_HELPER_FAST_CALL |
			ZEND_JIT_HELPER_ARG1_NOALIAS | ZEND_JIT_HELPER_ARG1_NOCAPTURE |
			ZEND_JIT_HELPER_ARG2_NOALIAS | ZEND_JIT_HELPER_ARG2_NOCAPTURE |
			ZEND_JIT_HELPER_ARG3_NOALIAS | ZEND_JIT_HELPER_ARG3_NOCAPTURE,
			Type::getInt32Ty(llvm_ctx.context),
			llvm_ctx.zval_ptr_type,
			llvm_ctx.zval_ptr_type,
			llvm_ctx.zval_ptr_type,
			NULL,
			NULL);

		if (op1_info & (MAY_BE_IN_REG)) {
			op1_addr = zend_jit_reload_from_reg(llvm_ctx, op1_ssa_var, op1_info);
		}
		if (op2_info & (MAY_BE_IN_REG)) {
			op2_addr = zend_jit_reload_from_reg(llvm_ctx, op2_ssa_var, op2_info);
		}
		CallInst *call = llvm_ctx.builder.CreateCall3(_helper,
			result_addr ? result_addr : zend_jit_load_tmp_zval(llvm_ctx, result_op->var),
			op1_addr,
			op2_addr);
		call->setCallingConv(CallingConv::X86_FastCall);

		if (result_info & (MAY_BE_IN_REG)) {
			zend_jit_reload_to_reg(llvm_ctx,
				result_addr ? result_addr : zend_jit_load_tmp_zval(llvm_ctx, result_op->var),
				result_ssa_var, result_info);
		}

		if (op1_addr != result_addr) {
			if (!zend_jit_free_operand(llvm_ctx, op1_op_type, orig_op1_addr, NULL, op1_ssa_var, op1_info, lineno)) {
				return 0;
			}
		}
		if (!zend_jit_free_operand(llvm_ctx, op2_op_type, orig_op2_addr, NULL, op2_ssa_var, op2_info, lineno)) {
			return 0;
		}
		
		if (bb_finish) {
			llvm_ctx.builder.CreateBr(bb_finish);
		}
	}

	if (bb_finish) {
		llvm_ctx.builder.SetInsertPoint(bb_finish);
	}

	return 1;
}
/* }}} */

/* {{{ static int zend_jit_math_op */
static int zend_jit_math_op(zend_llvm_ctx    &llvm_ctx,
                            zend_op_array    *op_array,
                            zend_op          *opline)
{
	Value *orig_op1_addr = NULL;
	Value *orig_op2_addr = NULL;
	Value *op1_addr = NULL;
	Value *op2_addr = NULL;

	if (!zend_jit_load_operands(llvm_ctx, op_array, opline, &orig_op1_addr, &orig_op2_addr)) return 0;
	if (OP1_OP_TYPE() == IS_VAR || OP1_OP_TYPE() == IS_CV) {
		op1_addr = zend_jit_deref(llvm_ctx, orig_op1_addr, OP1_SSA_VAR(), OP1_INFO());
	} else {
		op1_addr = orig_op1_addr;
	}
	if (OP2_OP_TYPE() == IS_VAR || OP2_OP_TYPE() == IS_CV) {
		op2_addr = zend_jit_deref(llvm_ctx, orig_op2_addr, OP2_SSA_VAR(), OP2_INFO());
	} else {
		op2_addr = orig_op2_addr;
	}

	return zend_jit_math(llvm_ctx,
			orig_op1_addr,
			op1_addr,
			OP1_OP_TYPE(),
			OP1_OP(),
			OP1_SSA_VAR(),
			OP1_INFO(),
			orig_op2_addr,
			op2_addr,
			OP2_OP_TYPE(),
			OP2_OP(),
			OP2_SSA_VAR(),
			OP2_INFO(),
			NULL,
			RES_OP(),
			RES_SSA_VAR(),
			RES_INFO(),
			SAME_CVs(opline),
			opline->lineno,
			opline->opcode,
			opline,
			1);
}
/* }}} */

/* {{{ static int zend_jit_long_math_op */
static int zend_jit_long_math_op(zend_llvm_ctx    &llvm_ctx,
                                 zend_op_array    *op_array,
                                 zend_op          *opline)
{
	Value *orig_op1_addr = NULL;
	Value *orig_op2_addr = NULL;
	Value *op1_addr = NULL;
	Value *op2_addr = NULL;

	if (!zend_jit_load_operands(llvm_ctx, op_array, opline, &orig_op1_addr, &orig_op2_addr)) return 0;
	if (OP1_OP_TYPE() == IS_VAR || OP1_OP_TYPE() == IS_CV) {
		op1_addr = zend_jit_deref(llvm_ctx, orig_op1_addr, OP1_SSA_VAR(), OP1_INFO());
	} else {
		op1_addr = orig_op1_addr;
	}
	if (OP2_OP_TYPE() == IS_VAR || OP2_OP_TYPE() == IS_CV) {
		op2_addr = zend_jit_deref(llvm_ctx, orig_op2_addr, OP2_SSA_VAR(), OP2_INFO());
	} else {
		op2_addr = orig_op2_addr;
	}

	return zend_jit_long_math(llvm_ctx,
			orig_op1_addr,
			op1_addr,
			OP1_OP_TYPE(),
			OP1_OP(),
			OP1_SSA_VAR(),
			OP1_INFO(),
			orig_op2_addr,
			op2_addr,
			OP2_OP_TYPE(),
			OP2_OP(),
			OP2_SSA_VAR(),
			OP2_INFO(),
			NULL,
			RES_OP(),
			RES_SSA_VAR(),
			RES_INFO(),
			SAME_CVs(opline),
			opline->lineno,
			opline->opcode,
			opline);
}
/* }}} */

/* {{{ static int zend_jit_bw_not */
static int zend_jit_bw_not(zend_llvm_ctx    &llvm_ctx,
                           zend_op_array    *op_array,
                           zend_op          *opline)
{
	Value *orig_op1_addr = NULL;
	Value *op1_addr = NULL;
	Value *op1_val;
	BasicBlock *bb_slow_path = NULL;
	BasicBlock *bb_finish = NULL;

	orig_op1_addr = zend_jit_load_operand(llvm_ctx,
			OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO(), 0, opline);
	if (OP1_OP_TYPE() == IS_VAR || OP1_OP_TYPE() == IS_CV) {
		op1_addr = zend_jit_deref(llvm_ctx, orig_op1_addr, OP1_SSA_VAR(), OP1_INFO());
	} else {
		op1_addr = orig_op1_addr;
	}
	
	if (OP1_MAY_BE(MAY_BE_LONG)) {
		if (OP1_MAY_BE(MAY_BE_ANY-MAY_BE_LONG)) {
			BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_slow_path = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			Value *op1_type = zend_jit_load_type_c(llvm_ctx, op1_addr, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO());
			zend_jit_expected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(
					op1_type,
					llvm_ctx.builder.getInt8(IS_LONG)),
				bb_follow,
				bb_slow_path);
			llvm_ctx.builder.SetInsertPoint(bb_follow);
		}
		op1_val = zend_jit_load_lval_c(llvm_ctx, op1_addr, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO());
		Value *res = llvm_ctx.builder.CreateNot(op1_val);
		Value *result = zend_jit_load_tmp_zval(llvm_ctx, RES_OP()->var);
		zend_jit_save_zval_type_info(llvm_ctx, result, RES_SSA_VAR(), RES_INFO(), llvm_ctx.builder.getInt32(IS_LONG));
		zend_jit_save_zval_lval(llvm_ctx, result, RES_SSA_VAR(), RES_INFO(), res);
		if (bb_finish) {
			llvm_ctx.builder.CreateBr(bb_finish);
		}

	}

	if (OP1_MAY_BE(MAY_BE_ANY-MAY_BE_LONG)) {
		if (bb_slow_path) {
			llvm_ctx.builder.SetInsertPoint(bb_slow_path);
		}
		
		Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)bitwise_not_function,
			"bitwise_not_function",
			ZEND_JIT_HELPER_FAST_CALL |
			ZEND_JIT_HELPER_ARG1_NOALIAS | ZEND_JIT_HELPER_ARG1_NOCAPTURE |
			ZEND_JIT_HELPER_ARG2_NOALIAS | ZEND_JIT_HELPER_ARG2_NOCAPTURE,
			Type::getInt32Ty(llvm_ctx.context),
			llvm_ctx.zval_ptr_type,
			llvm_ctx.zval_ptr_type,
			NULL,
			NULL,
			NULL);

		if (OP1_MAY_BE(MAY_BE_IN_REG)) {
			op1_addr = zend_jit_reload_from_reg(llvm_ctx, OP1_SSA_VAR(), OP1_INFO());
		}
		CallInst *call = llvm_ctx.builder.CreateCall2(_helper,
			zend_jit_load_tmp_zval(llvm_ctx, RES_OP()->var),
			op1_addr);
		call->setCallingConv(CallingConv::X86_FastCall);

		if (RES_MAY_BE(MAY_BE_IN_REG)) {
			zend_jit_reload_to_reg(llvm_ctx,
				zend_jit_load_tmp_zval(llvm_ctx, RES_OP()->var),
				RES_SSA_VAR(), RES_INFO());
		}

		if (bb_finish) {
			llvm_ctx.builder.CreateBr(bb_finish);
		}
	}

	if (bb_finish) {
		llvm_ctx.builder.SetInsertPoint(bb_finish);
	}

	if (!zend_jit_free_operand(llvm_ctx, OP1_OP_TYPE(), orig_op1_addr, NULL, OP1_SSA_VAR(), OP1_INFO(), opline->lineno)) {
		return 0;
	}

//???	if (OP1_MAY_BE(MAY_BE_RC1) && OP1_MAY_BE(MAY_BE_OBJECT|MAY_BE_RESOURCE|MAY_BE_ARRAY_OF_OBJECT|MAY_BE_ARRAY_OF_RESOURCE)) {
		JIT_CHECK(zend_jit_check_exception(llvm_ctx, opline));
//???	}

	llvm_ctx.valid_opline = 0;

	return 1;
}
/* }}} */

/* {{{ static int zend_jit_bool */
static int zend_jit_bool(zend_llvm_ctx    &llvm_ctx,
                         zend_op_array    *op_array,
                         zend_op          *opline,
                         bool              neg)
{
	BasicBlock *bb_true = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	BasicBlock *bb_false = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	BasicBlock *bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	Value *result;

	zend_jit_jmpznz(llvm_ctx, op_array, opline, bb_true, bb_false, -1);

	llvm_ctx.builder.SetInsertPoint(bb_true);
	result = zend_jit_load_tmp_zval(llvm_ctx, RES_OP()->var);
	zend_jit_save_zval_type_info(llvm_ctx, result, RES_SSA_VAR(), RES_INFO(), llvm_ctx.builder.getInt32(neg ? IS_TRUE : IS_FALSE));
	llvm_ctx.builder.CreateBr(bb_finish);

	llvm_ctx.builder.SetInsertPoint(bb_false);
	result = zend_jit_load_tmp_zval(llvm_ctx, RES_OP()->var);
	zend_jit_save_zval_type_info(llvm_ctx, result, RES_SSA_VAR(), RES_INFO(), llvm_ctx.builder.getInt32(neg ? IS_FALSE : IS_TRUE));
	llvm_ctx.builder.CreateBr(bb_finish);

	llvm_ctx.builder.SetInsertPoint(bb_finish);

//???	if (OP1_MAY_BE(MAY_BE_RC1) && OP1_MAY_BE(MAY_BE_OBJECT|MAY_BE_RESOURCE|MAY_BE_ARRAY_OF_OBJECT|MAY_BE_ARRAY_OF_RESOURCE)) {
		JIT_CHECK(zend_jit_check_exception(llvm_ctx, opline));
//???	}

	llvm_ctx.valid_opline = 0;

	return 1;
}
/* }}} */

/* {{{ static int zend_jit_qm_assign */
static int zend_jit_qm_assign(zend_llvm_ctx    &llvm_ctx,
                              zend_op_array    *op_array,
                              zend_op          *opline)
{
	Value *op1_addr;
	Value *op1_type;
	Value *ret;
	BasicBlock *bb_finish = NULL;
	
//???	if (RES_MAY_BE(MAY_BE_IN_REG)) {
//???		if (OP1_MAY_BE(MAY_BE_DOUBLE)) {
//???			zend_jit_save_to_reg(llvm_ctx, RES_SSA_VAR(), RES_INFO(),
//???				zend_jit_load_dval(llvm_ctx, op1_addr, OP1_SSA_VAR(), OP1_INFO()));
//???		} else {
//???			zend_jit_save_to_reg(llvm_ctx, RES_SSA_VAR(), RES_INFO(),
//???				zend_jit_load_lval_c(llvm_ctx, op1_addr, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO()));
//???		}
//???	} else { 

		op1_addr = zend_jit_load_operand(llvm_ctx,
				OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO(), 0, opline);
		op1_type = zend_jit_load_type_info_c(llvm_ctx, op1_addr, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO());
		ret = zend_jit_load_slot(llvm_ctx, RES_OP()->var);
		if (OP1_OP_TYPE() == IS_VAR || OP1_OP_TYPE() == IS_CV) {
			if (OP1_MAY_BE(MAY_BE_REF)) {
				BasicBlock *bb_ref = NULL;
				BasicBlock *bb_noref = NULL;

				if (OP1_MAY_BE(MAY_BE_RC1 | MAY_BE_RCN)) {
					bb_ref = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					bb_noref = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					zend_jit_unexpected_br(llvm_ctx,
						llvm_ctx.builder.CreateICmpEQ(
							op1_type,
							llvm_ctx.builder.getInt32(IS_REFERENCE_EX)),
						bb_ref,
						bb_noref);
					llvm_ctx.builder.SetInsertPoint(bb_ref);
				}
				Value *counted = zend_jit_load_counted(llvm_ctx, op1_addr, OP1_SSA_VAR(), OP1_INFO());
				Value *ref = zend_jit_load_reference(llvm_ctx, counted);
				Value *ref_type = zend_jit_load_type_info_c(llvm_ctx, ref, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO());
				zend_jit_copy_value(llvm_ctx, ret, 0, RES_SSA_VAR(), RES_INFO(),
						ref, ref_type, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO());
				zend_jit_try_addref(llvm_ctx, ref, ref_type, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO());
				// INFO changed to IS_OBJECT to avoid Z_REFCOUNTED() check
				if (!zend_jit_free_operand(llvm_ctx, OP1_OP_TYPE(), op1_addr, op1_type, OP1_SSA_VAR(), MAY_BE_OBJECT, opline->lineno)) return 0;
				if (bb_noref) {
					if (!bb_finish) {
						bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					}
					llvm_ctx.builder.CreateBr(bb_finish);
					llvm_ctx.builder.SetInsertPoint(bb_noref);
				}
			}
		}

		if (OP1_MAY_BE(MAY_BE_RC1 | MAY_BE_RCN)) {
			zend_jit_copy_value(llvm_ctx, ret, 0, RES_SSA_VAR(), RES_INFO(),
					op1_addr, op1_type, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO());
			if (OP1_OP_TYPE() == IS_CONST) {
				if (UNEXPECTED(Z_COPYABLE_P(RT_CONSTANT(llvm_ctx.op_array, *OP1_OP())))) {
					zend_jit_copy_ctor_func(llvm_ctx, ret, opline->lineno);
				}
			} else if (OP1_OP_TYPE() == IS_CV) {
				if (OP1_MAY_BE(MAY_BE_STRING|MAY_BE_ARRAY|MAY_BE_OBJECT|MAY_BE_RESOURCE)) {
					BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					if (!bb_finish) {
						bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					}
					zend_jit_expected_br(llvm_ctx,
						llvm_ctx.builder.CreateICmpNE(
							llvm_ctx.builder.CreateAnd(
								op1_type,
								llvm_ctx.builder.getInt32(IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT)),
						llvm_ctx.builder.getInt32(0)),
						bb_follow,
						bb_finish);
					llvm_ctx.builder.SetInsertPoint(bb_follow);
					zend_jit_addref(llvm_ctx,
						zend_jit_load_counted(llvm_ctx,
							ret, RES_SSA_VAR(), RES_INFO()));
				}
			}
		}
		if (bb_finish) {
			llvm_ctx.builder.CreateBr(bb_finish);
			llvm_ctx.builder.SetInsertPoint(bb_finish);
		}
//???	}

	llvm_ctx.valid_opline = 0;
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_assign_to_variable */
static int zend_jit_assign_to_variable(zend_llvm_ctx    &llvm_ctx,
                                       zend_op_array    *op_array,
                                       Value            *op1_addr,
                                       uint32_t          op1_info,
                                       int               op1_ssa_var,
                                       uint32_t          op1_def_info,
                                       int               op1_def_ssa_var,
                                       zend_uchar        op1_type,
                                       znode_op         *op1,
                                       Value            *op2_addr,
                                       uint32_t          op2_info,
                                       int               op2_ssa_var,
                                       zend_uchar        op2_type,
                                       znode_op         *op2,
                                       zend_op          *opline)
{
	BasicBlock *bb_common = NULL;
	BasicBlock *bb_return = NULL;
	BasicBlock *bb_follow;
	Value *op1_type_info = NULL;
	Value *op2_type_info = NULL;
	PHI_DCL(common, 5)
	PHI_DCL(rc_addr, 2)
	PHI_DCL(rc_type_info, 2)

	op1_type_info = zend_jit_load_type_info_c(llvm_ctx, op1_addr, op1_type, op1, op1_ssa_var, op1_info);
	op2_type_info = zend_jit_load_type_info_c(llvm_ctx, op2_addr, op2_type, op2, op2_ssa_var, op2_info);

	/* op1 may by IS_UNDEFINED */
	if ((op1_info & MAY_BE_UNDEF) && ((op1_info & MAY_BE_ANY) == MAY_BE_NULL)) {
		op1_info &= ~MAY_BE_NULL;
	}

	if (op1_info & (MAY_BE_STRING | MAY_BE_ARRAY | MAY_BE_OBJECT | MAY_BE_RESOURCE | MAY_BE_REF)) {
		if (op1_info & (MAY_BE_ANY - (MAY_BE_OBJECT | MAY_BE_RESOURCE))) {
			//JIT: if (UNEXPECTED(Z_REFCOUNTED_P(variable_ptr))) {	
			PHI_ADD(common, op1_addr);
			bb_common = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			zend_jit_unexpected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpNE(
					llvm_ctx.builder.CreateAnd(
						op1_type_info,
						llvm_ctx.builder.getInt32(IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT)),
					llvm_ctx.builder.getInt32(0)),
					bb_follow,
					bb_common);
				llvm_ctx.builder.SetInsertPoint(bb_follow);						
		}
		
		if (op1_info & MAY_BE_REF) {
			BasicBlock *bb_rc = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			if (op1_info & (MAY_BE_RC1|MAY_BE_RCN)) {
				PHI_ADD(rc_addr, op1_addr);
				PHI_ADD(rc_type_info, op1_type_info);
				//JIT: if (Z_ISREF_P(variable_ptr)) {
				BasicBlock *bb_ref = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				zend_jit_unexpected_br(llvm_ctx,
					llvm_ctx.builder.CreateICmpEQ(
						op1_type_info,
						llvm_ctx.builder.getInt32(IS_REFERENCE_EX)),
					bb_ref,
					bb_rc);
				llvm_ctx.builder.SetInsertPoint(bb_ref);
			}
			//JIT: variable_ptr = Z_REFVAL_P(variable_ptr);
			Value *counted = zend_jit_load_counted(llvm_ctx, op1_addr, op1_ssa_var, op1_info);
			Value *ref_addr = zend_jit_load_reference(llvm_ctx, counted);
			Value *ref_type_info = zend_jit_load_type_info(llvm_ctx, ref_addr, -1, MAY_BE_ANY);

			// JIT: if (EXPECTED(!Z_REFCOUNTED_P(variable_ptr))) {
			if (!bb_common) {
				bb_common = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
            PHI_ADD(common, ref_addr);
            PHI_ADD(rc_addr, ref_addr);
            PHI_ADD(rc_type_info, ref_type_info);
			zend_jit_unexpected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpNE(
					llvm_ctx.builder.CreateAnd(
						ref_type_info,
						llvm_ctx.builder.getInt32(IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT)),
					llvm_ctx.builder.getInt32(0)),
					bb_rc,
					bb_common);
			llvm_ctx.builder.SetInsertPoint(bb_rc);
			PHI_SET(rc_addr, op1_addr, llvm_ctx.zval_ptr_type);
			PHI_SET(rc_type_info, op1_type_info, Type::getInt32Ty(llvm_ctx.context));
		}

		if (op1_info & MAY_BE_OBJECT) {
			//JIT: if (Z_TYPE_P(variable_ptr) == IS_OBJECT &&
			//JIT:     UNEXPECTED(Z_OBJ_HANDLER_P(variable_ptr, set) != NULL)) {
			//JIT: Z_OBJ_HANDLER_P(variable_ptr, set)(variable_ptr, value TSRMLS_CC);
			//JIT: return variable_ptr;
		}

		if ((op2_type & (IS_VAR|IS_CV)) && op1_addr && op2_addr) {
			//JIT: if (variable_ptr == value) return variable_ptr
			bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			if (!bb_return) {
				bb_return = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			zend_jit_unexpected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(
					op1_addr,
					op2_addr),
				bb_return,
				bb_follow);
			llvm_ctx.builder.SetInsertPoint(bb_follow);
		}

		//JIT: garbage = Z_COUNTED_P(variable_ptr);
		Value *garbage = zend_jit_load_counted(llvm_ctx, op1_addr, op1_ssa_var, op1_info);

		//JIT: if (--GC_REFCOUNT(garbage) == 0) {
		Value *refcount = zend_jit_delref(llvm_ctx, garbage);
		BasicBlock *bb_rcn = NULL;
//???		if (op1_info & (MAY_BE_RC1|MAY_BE_REF)) {
//???			if (op1_info & (MAY_BE_RCN|MAY_BE_REF)) {
				BasicBlock *bb_rc1 = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				bb_rcn = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				zend_jit_unexpected_br(llvm_ctx,
					llvm_ctx.builder.CreateICmpEQ(
						refcount,
						llvm_ctx.builder.getInt32(0)),
					bb_rc1,
					bb_rcn);
					llvm_ctx.builder.SetInsertPoint(bb_rc1);
//???			}
			//JIT: ZVAL_COPY_VALUE(variable_ptr, value);
			zend_jit_copy_value(llvm_ctx, op1_addr, op1_info, op1_def_ssa_var, op1_def_info,
					op2_addr, op2_type_info, op2_type, op2, op2_ssa_var, op2_info);
			if (op2_type == IS_CONST) {
				if (UNEXPECTED(Z_COPYABLE_P(RT_CONSTANT(llvm_ctx.op_array, *op2)))) {
					// JIT: zval_copy_ctor_func(variable_ptr);
					zend_jit_copy_ctor_func(llvm_ctx, op1_addr, opline->lineno);
				}
			} else if (op2_type != IS_TMP_VAR) {
				if (op2_info & (MAY_BE_STRING|MAY_BE_ARRAY|MAY_BE_OBJECT|MAY_BE_RESOURCE)) {
					BasicBlock *bb_norc = NULL;
					if (op2_info & (MAY_BE_ANY - (MAY_BE_OBJECT|MAY_BE_RESOURCE))) {
						// JIT: if (UNEXPECTED(Z_OPT_REFCOUNTED_P(variable_ptr))) {
						BasicBlock *bb_rc = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
						bb_norc = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
						zend_jit_expected_br(llvm_ctx,
							llvm_ctx.builder.CreateICmpNE(
								llvm_ctx.builder.CreateAnd(
									op2_type_info,
									llvm_ctx.builder.getInt32(IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT)),
							llvm_ctx.builder.getInt32(0)),
							bb_rc,
							bb_norc);
						llvm_ctx.builder.SetInsertPoint(bb_rc);
					}
					// JIT: Z_ADDREF_P(variable_ptr);
					zend_jit_addref(llvm_ctx,
						zend_jit_load_counted(llvm_ctx,
							op2_addr, op2_ssa_var, op2_info));
					if (bb_norc) {
						llvm_ctx.builder.CreateBr(bb_norc);
						llvm_ctx.builder.SetInsertPoint(bb_norc);
					}
				}
			}
			// JIT: _zval_dtor_func_for_ptr(garbage ZEND_FILE_LINE_CC);
			zend_jit_zval_dtor_func_for_ptr(llvm_ctx, garbage, opline->lineno);
			// JIT: return variable_ptr;
			if (!bb_return) {
				bb_return = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(bb_return);
//???		}

//???		if (op1_info & (MAY_BE_RCN|MAY_BE_REF)) {
			if (bb_rcn) {
				llvm_ctx.builder.SetInsertPoint(bb_rcn);
			}
			if (op1_info & (MAY_BE_ARRAY|MAY_BE_OBJECT)) {
				//JIT: if ((Z_COLLECTABLE_P(variable_ptr))
				bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				if (!bb_common) {
					bb_common = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				}
				PHI_ADD(common, op1_addr);
				zend_jit_expected_br(llvm_ctx,
					llvm_ctx.builder.CreateICmpNE(
						llvm_ctx.builder.CreateAnd(
							op1_type_info,
							llvm_ctx.builder.getInt32(IS_TYPE_COLLECTABLE << Z_TYPE_FLAGS_SHIFT)),
					llvm_ctx.builder.getInt32(0)),
					bb_follow,
					bb_common);
				llvm_ctx.builder.SetInsertPoint(bb_follow);
				//JIT: if (UNEXPECTED(!GC_INFO(garbage))) {
				bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				if (!bb_common) {
					bb_common = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				}
				PHI_ADD(common, op1_addr);
				zend_jit_expected_br(llvm_ctx,
					llvm_ctx.builder.CreateICmpEQ(
						llvm_ctx.builder.CreateAlignedLoad(
							zend_jit_GEP(
								llvm_ctx,
								garbage,
								offsetof(zend_refcounted, gc.u.v.gc_info),
								PointerType::getUnqual(Type::getInt16Ty(llvm_ctx.context))), 2),
						llvm_ctx.builder.getInt16(0)),
					bb_follow,
					bb_common);
				llvm_ctx.builder.SetInsertPoint(bb_follow);
				//JIT: gc_possible_root(garbage TSRMLS_CC);
				zend_jit_gc_possible_root(llvm_ctx, garbage);
    		}
    		PHI_ADD(common, op1_addr);
    		if (bb_common) {
				llvm_ctx.builder.CreateBr(bb_common);
    		}
//???		}
	}

	if (bb_common) {
		llvm_ctx.builder.SetInsertPoint(bb_common);
		PHI_SET(common, op1_addr, llvm_ctx.zval_ptr_type);
	}		

	//JIT: ZVAL_COPY_VALUE(variable_ptr, value);
	zend_jit_copy_value(llvm_ctx, op1_addr, op1_info, op1_def_ssa_var, op1_def_info,
			op2_addr, op2_type_info, op2_type, op2, op2_ssa_var, op2_info);
	if (op2_type == IS_CONST) {
		if (UNEXPECTED(Z_COPYABLE_P(RT_CONSTANT(llvm_ctx.op_array, *op2)))) {
			//JIT: zval_copy_ctor_func(variable_ptr);
			zend_jit_copy_ctor_func(llvm_ctx, op1_addr, opline->lineno);
		}
	} else if (op2_type != IS_TMP_VAR) {
		if (op2_info & (MAY_BE_STRING|MAY_BE_ARRAY|MAY_BE_OBJECT|MAY_BE_RESOURCE)) {
			if (op2_info & (MAY_BE_ANY - (MAY_BE_OBJECT|MAY_BE_RESOURCE))) {
				//JIT: if (UNEXPECTED(Z_OPT_REFCOUNTED_P(variable_ptr))) {
				bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				if (!bb_return) {
					bb_return = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				}
				zend_jit_expected_br(llvm_ctx,
					llvm_ctx.builder.CreateICmpNE(
						llvm_ctx.builder.CreateAnd(
							op2_type_info,
							llvm_ctx.builder.getInt32(IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT)),
					llvm_ctx.builder.getInt32(0)),
					bb_follow,
					bb_return);
				llvm_ctx.builder.SetInsertPoint(bb_follow);
			}
			//JIT: Z_ADDREF_P(variable_ptr);
			zend_jit_addref(llvm_ctx,
				zend_jit_load_counted(llvm_ctx,
					op2_addr, op2_ssa_var, op2_info));
		}
	}
	
	// JIT: return variable_ptr;
	if (bb_return) {
		llvm_ctx.builder.CreateBr(bb_return);
		llvm_ctx.builder.SetInsertPoint(bb_return);
	}		

	if (RETURN_VALUE_USED(opline)) {
//???TODO: not op2_addr but op1_addr
		// JIT: ZVAL_COPY_VALUE(EX_VAR(RES_OP()->var), value);
		Value *ret = zend_jit_load_slot(llvm_ctx, RES_OP()->var);

		zend_jit_copy_value(llvm_ctx, ret, 0, RES_SSA_VAR(), RES_INFO(),
			op2_addr, op2_type_info, op2_type, op2, op2_ssa_var, op2_info);

		if (op2_info & (MAY_BE_STRING|MAY_BE_ARRAY|MAY_BE_OBJECT|MAY_BE_RESOURCE|MAY_BE_REF)) {
			BasicBlock *bb_rc = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			BasicBlock *bb_norc = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			zend_jit_expected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpNE(
					llvm_ctx.builder.CreateAnd(
						op2_type_info,
						llvm_ctx.builder.getInt32(IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT)),
				llvm_ctx.builder.getInt32(0)),
				bb_rc,
				bb_norc);
			llvm_ctx.builder.SetInsertPoint(bb_rc);
			//JIT: Z_ADDREF_P(variable_ptr);
			zend_jit_addref(llvm_ctx,
				zend_jit_load_counted(llvm_ctx,
					op2_addr, op2_ssa_var, op2_info));
			llvm_ctx.builder.CreateBr(bb_norc);
			llvm_ctx.builder.SetInsertPoint(bb_norc);
		}
	}
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_update_reg_value */
static int zend_jit_update_reg_value(zend_llvm_ctx    &llvm_ctx,
                                     uint32_t          var,
                                     Value            *zval_ptr,
                                     int               ssa_var,
                                     uint32_t          info,
                                     int               def_ssa_var,
                                     uint32_t          def_info)
{
	if (info & MAY_BE_IN_REG) {
		if (def_info & MAY_BE_IN_REG) {
			if (llvm_ctx.reg[ssa_var] != llvm_ctx.reg[def_ssa_var]) {
				llvm_ctx.builder.CreateAlignedStore(
					llvm_ctx.builder.CreateAlignedLoad(
						llvm_ctx.reg[ssa_var], 4),
					llvm_ctx.reg[def_ssa_var], 4);
				
			}
		} else {
			if (!zval_ptr) {
				zval_ptr = zend_jit_load_slot(llvm_ctx, var);
			}
			if (info & (MAY_BE_NULL | MAY_BE_FALSE | MAY_BE_TRUE)) {
				zend_jit_save_zval_type_info(llvm_ctx, zval_ptr, def_ssa_var, def_info,
					llvm_ctx.builder.CreateAlignedLoad(
						llvm_ctx.reg[ssa_var], 4));
			} else if (info & MAY_BE_LONG) {
				zend_jit_save_zval_lval(llvm_ctx, zval_ptr, def_ssa_var, def_info,
					llvm_ctx.builder.CreateAlignedLoad(
						llvm_ctx.reg[ssa_var], 4));
			} else if (info & MAY_BE_DOUBLE) {
				zend_jit_save_zval_dval(llvm_ctx, zval_ptr, def_ssa_var, def_info,
					llvm_ctx.builder.CreateAlignedLoad(
						llvm_ctx.reg[ssa_var], 4));
			} else {
				zend_jit_save_zval_ptr(llvm_ctx, zval_ptr, def_ssa_var, def_info,
					llvm_ctx.builder.CreateAlignedLoad(
						llvm_ctx.reg[ssa_var], 4));
			}
		}
	} else if (def_info & MAY_BE_IN_REG) {
		if (!zval_ptr) {
			zval_ptr = zend_jit_load_slot(llvm_ctx, var);
		}
		if (def_info & (MAY_BE_NULL | MAY_BE_FALSE | MAY_BE_TRUE)) {
			llvm_ctx.builder.CreateAlignedStore(
				zend_jit_load_type_info(llvm_ctx, zval_ptr, ssa_var, info),
				llvm_ctx.reg[def_ssa_var], 4);
		} else if (def_info & MAY_BE_LONG) {
			llvm_ctx.builder.CreateAlignedStore(
				zend_jit_load_lval(llvm_ctx, zval_ptr, ssa_var, info),
				llvm_ctx.reg[def_ssa_var], 4);
		} else if (def_info & MAY_BE_DOUBLE) {
			llvm_ctx.builder.CreateAlignedStore(
				zend_jit_load_dval(llvm_ctx, zval_ptr, ssa_var, info),
				llvm_ctx.reg[def_ssa_var], 4);
		} else {
			llvm_ctx.builder.CreateAlignedStore(
				zend_jit_load_ptr(llvm_ctx, zval_ptr, ssa_var, info),
				llvm_ctx.reg[def_ssa_var], 4);
		}
	}

	return 1;
}
/* }}} */

/* {{{ static int zend_jit_assign */
static int zend_jit_assign(zend_llvm_ctx    &llvm_ctx,
                           zend_op_array    *op_array,
                           zend_op          *opline)
{
	Value *orig_op2_addr = NULL;
	Value *op1_addr = NULL;
	Value *op2_addr = NULL;
	BasicBlock *bb_finish = NULL;

	// JIT:??? variable_ptr = GET_OP1_ZVAL_PTR_PTR_UNDEF(BP_VAR_W);
	if (OP1_OP_TYPE() == IS_CV) {
		op1_addr = zend_jit_load_slot(llvm_ctx, OP1_OP()->var);
	} else if (OP1_OP_TYPE() == IS_VAR) {
//TODO: ???
		return zend_jit_handler(llvm_ctx, opline);
	} else {
		ASSERT_NOT_REACHED();
	}

	// JIT: value = GET_OP2_ZVAL_PTR_DEREF(BP_VAR_R);
	orig_op2_addr = zend_jit_load_operand(llvm_ctx, OP2_OP_TYPE(), OP2_OP(), OP2_SSA_VAR(), OP2_INFO(), 0, opline);

	if (OP1_OP_TYPE() == IS_VAR && OP1_MAY_BE(MAY_BE_ERROR)) {
		// JIT: if (OP1_TYPE == IS_VAR && UNEXPECTED(variable_ptr == &EG(error_zval)))
		BasicBlock *bb_error = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		zend_jit_unexpected_br(llvm_ctx,
			llvm_ctx.builder.CreateICmpEQ(
				op1_addr,
				llvm_ctx._EG_error_zval),
			bb_error,
			bb_follow);
		llvm_ctx.builder.SetInsertPoint(bb_error);

		// JIT: FREE_OP2();
		if (!zend_jit_free_operand(llvm_ctx, OP2_OP_TYPE(), orig_op2_addr, NULL, OP2_SSA_VAR(), OP2_INFO(), opline->lineno)) {
			return 0;
		}
		if (RETURN_VALUE_USED(opline)) {
			// JIT: ZVAL_NULL(EX_VAR(RES_OP()->var));
			Value *ret = zend_jit_load_slot(llvm_ctx, RES_OP()->var);
			zend_jit_save_zval_type_info(llvm_ctx, ret, RES_SSA_VAR(), RES_INFO(), llvm_ctx.builder.getInt32(IS_NULL));
		}
		llvm_ctx.builder.CreateBr(bb_finish);
		llvm_ctx.builder.SetInsertPoint(bb_follow);
	}

	if (OP2_OP_TYPE() == IS_VAR || OP2_OP_TYPE() == IS_CV) {
		// TODO: deref for IS_CV mau be optimized
		op2_addr = zend_jit_deref(llvm_ctx, orig_op2_addr, OP2_SSA_VAR(), OP2_INFO());
	} else {
		op2_addr = orig_op2_addr;
	}

	zend_jit_assign_to_variable(
		llvm_ctx,
		op_array,
		op1_addr,
		OP1_INFO(),
		OP1_SSA_VAR(),
		OP1_DEF_INFO(),
		OP1_DEF_SSA_VAR(),
		OP1_OP_TYPE(),
		OP1_OP(),
		op2_addr,
		OP2_INFO(),
		OP2_SSA_VAR(),
		OP2_OP_TYPE(),
		OP2_OP(),
		opline);

	if (OP2_OP_TYPE() == IS_CV) {
		zend_jit_update_reg_value(
			llvm_ctx,
			opline->op2.var,
			op2_addr,
			OP2_SSA_VAR(),
			OP2_INFO(),
			OP2_DEF_SSA_VAR(),
			OP2_DEF_INFO());
	} else if (OP2_OP_TYPE() == IS_VAR) {
		if (!zend_jit_free_operand(llvm_ctx, OP2_OP_TYPE(), orig_op2_addr, NULL, OP2_SSA_VAR(), OP2_INFO(), opline->lineno)) {
			return 0;
		}
	}

	if (bb_finish) {
		llvm_ctx.builder.CreateBr(bb_finish);
		llvm_ctx.builder.SetInsertPoint(bb_finish);
	}
	
	//JIT: CHECK_EXCEPTION
	if (OP1_INFO() & (MAY_BE_ARRAY|MAY_BE_OBJECT|MAY_BE_RESOURCE)) {
		JIT_CHECK(zend_jit_check_exception(llvm_ctx, opline));
	}

	llvm_ctx.valid_opline = 0;
	return 1;
}
/* }}} */

#if 0 // TODO: reimplement FAST_CONCATR and ROPE_* ???
/* {{{ static int zend_jit_add_string */
static int zend_jit_add_string(zend_llvm_ctx    &llvm_ctx,
                               zend_op_array    *op_array,
                               zend_op          *opline)
{
	Value *result_addr = zend_jit_load_var(llvm_ctx, RES_OP()->var);

	if (OP1_OP_TYPE() == IS_UNUSED) {
		zend_jit_save_zval_str(llvm_ctx, result_addr, RES_SSA_VAR(), RES_INFO(), llvm_ctx._CG_empty_string);
		zend_jit_save_zval_type_info(llvm_ctx, result_addr, RES_SSA_VAR(), RES_INFO(), llvm_ctx.builder.getInt32(IS_INTERNED_STRING_EX));
	}
	if (opline->opcode == ZEND_ADD_CHAR) {
		zend_jit_add_char_to_string(llvm_ctx, result_addr, RES_SSA_VAR(), RES_INFO(), result_addr, OP1_SSA_VAR(), OP1_INFO(), (char)Z_LVAL_P(RT_CONSTANT(llvm_ctx.op_array, *OP2_OP())), opline);
	} else if (OP2_OP_TYPE() == IS_CONST) {
		zend_jit_add_string_to_string(llvm_ctx, result_addr, RES_SSA_VAR(), RES_INFO(), result_addr, OP1_SSA_VAR(), OP1_INFO(), Z_STR_P(RT_CONSTANT(llvm_ctx.op_array, *OP2_OP())), opline);
	} else {
		BasicBlock *bb_string = NULL;
		BasicBlock *bb_copy = NULL;
		BasicBlock *bb_follow = NULL;
		Value      *op2_str, *str, *op2_type = NULL;
		Value      *op2 = zend_jit_load_operand(llvm_ctx, OP2_OP_TYPE(), OP2_OP(), OP2_SSA_VAR(), OP2_INFO(), 0, opline);

		zend_jit_make_printable_zval(
				llvm_ctx,
				op2,
				&op2_type,
				OP2_OP_TYPE(),
				OP2_OP(),
				OP2_SSA_VAR(),
				OP2_INFO(),
				&bb_string,
				&op2_str,
				&bb_copy,
				&str,
				opline);

		if (bb_string && bb_copy) {
			bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		}

		if (bb_string) {
			llvm_ctx.builder.SetInsertPoint(bb_string);
			zend_jit_add_var_to_string(llvm_ctx, result_addr, RES_SSA_VAR(), RES_INFO(), result_addr, OP1_SSA_VAR(), OP1_INFO(), op2_str, opline);
			if (bb_follow) {
				llvm_ctx.builder.CreateBr(bb_follow);
			}
		}

		if (bb_copy) {
			llvm_ctx.builder.SetInsertPoint(bb_copy);
			zend_jit_add_var_to_string(llvm_ctx, result_addr, RES_SSA_VAR(), RES_INFO(), result_addr, OP1_SSA_VAR(), OP1_INFO(), str, opline);
			zend_jit_string_release(llvm_ctx, str);
			if (bb_follow) {
				llvm_ctx.builder.CreateBr(bb_follow);
			}
		}

		if (bb_follow) {
			llvm_ctx.builder.SetInsertPoint(bb_follow);
		}

		if (zend_jit_free_operand(llvm_ctx, OP2_OP_TYPE(), op2, op2_type, OP2_SSA_VAR(), OP2_INFO(), opline->lineno) == 0) {
			return 0;
		}
	}

	llvm_ctx.valid_opline = 0;
	return 1;
}
/* }}} */
#endif

/* {{{ static int zend_jit_incdec */
static int zend_jit_incdec(zend_llvm_ctx    &llvm_ctx,
                           zend_op_array    *op_array,
                           zend_op          *opline)
{
	//JIT: var_ptr = GET_OP1_ZVAL_PTR_PTR(BP_VAR_RW);
	Value *should_free = NULL;
 	Value *op1_addr = zend_jit_load_operand_addr(llvm_ctx,
 		OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO(), 0, opline, 0, BP_VAR_RW,
 		&should_free);
	Value *op1_type_info = NULL;
	BasicBlock *bb_finish = NULL;
	Value *res;

	if (OP1_OP_TYPE() == IS_VAR) { //TODO: MAY_BE_STRING_OFFSET
		//JIT: if (UNEXPECTED(var_ptr == NULL)) {
		BasicBlock *bb_null = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_not_null = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		zend_jit_unexpected_br(llvm_ctx,
				llvm_ctx.builder.CreateIsNull(op1_addr),
					bb_null,
					bb_not_null);
		llvm_ctx.builder.SetInsertPoint(bb_null);
		//JIT: zend_error_noreturn(E_ERROR, "Cannot increment/decrement overloaded objects nor string offsets");
		zend_jit_error_noreturn(
			llvm_ctx,
			opline,
			E_ERROR,
			"Cannot increment/decrement overloaded objects nor string offsets");
		llvm_ctx.builder.SetInsertPoint(bb_not_null);
	}

 	if (OP1_MAY_BE(MAY_BE_LONG)) {
 		BasicBlock *bb_nolong = NULL;
 		if (OP1_MAY_BE(MAY_BE_ERROR | (MAY_BE_ANY - MAY_BE_LONG))) {
			//JIT: if (EXPECTED(Z_TYPE_P(var_ptr) == IS_LONG)) {
			BasicBlock *bb_long = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_nolong = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			op1_type_info = zend_jit_load_type_info_c(llvm_ctx, op1_addr, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO());
			zend_jit_expected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(
					op1_type_info,
					llvm_ctx.builder.getInt32(IS_LONG)),
				bb_long,
				bb_nolong);
			llvm_ctx.builder.SetInsertPoint(bb_long);
 		}

 		Value *val;
 		switch (opline->opcode) {
 			case ZEND_PRE_INC:
				//JIT: fast_increment_function(var_ptr);
				if (OP1_DEF_MAY_BE(MAY_BE_DOUBLE)) {
					val = zend_jit_load_lval_c(llvm_ctx, op1_addr, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO());
					Function *func = Intrinsic::getDeclaration(
						llvm_ctx.module,
						Intrinsic::sadd_with_overflow,
						ArrayRef<Type*>(LLVM_GET_LONG_TY(llvm_ctx.context)));
					Value *call = llvm_ctx.builder.CreateCall2(
						func,
						val,
						LLVM_GET_LONG(1));
					BasicBlock *bb_ok = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					BasicBlock *bb_overflow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					BasicBlock *bb_end = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					zend_jit_unexpected_br(llvm_ctx,
					    llvm_ctx.builder.CreateExtractValue(call, 1),
						bb_overflow,
						bb_ok);
					llvm_ctx.builder.SetInsertPoint(bb_overflow);
					if (!op1_addr) {
						op1_addr = zend_jit_load_slot(llvm_ctx, OP1_OP()->var);
					}
#if defined(__x86_64__)
					zend_jit_save_zval_lval(llvm_ctx,
						op1_addr, OP1_DEF_SSA_VAR(), OP1_DEF_INFO(),
						LLVM_GET_LONG(0x43e0000000000000));
#else
					zend_jit_save_zval_value(llvm_ctx,
						op1_addr,
						LLVM_GET_LONG(0x0),
						LLVM_GET_LONG(0x41e00000));
#endif
					zend_jit_save_zval_type_info(llvm_ctx, op1_addr, OP1_DEF_SSA_VAR(), OP1_DEF_INFO(), llvm_ctx.builder.getInt32(IS_DOUBLE));
					if (RETURN_VALUE_USED(opline)) {
						//JIT: ZVAL_COPY_VALUE(EX_VAR(RES_OP()->var), var_ptr);
						res = zend_jit_load_slot(llvm_ctx, RES_OP()->var);
#if defined(__x86_64__)
						zend_jit_save_zval_lval(llvm_ctx,
							res, RES_SSA_VAR(), RES_INFO(),
							LLVM_GET_LONG(0x43e0000000000000));
#else
						zend_jit_save_zval_value(llvm_ctx,
							res,
							LLVM_GET_LONG(0x0),
							LLVM_GET_LONG(0x41e00000));
#endif
						zend_jit_save_zval_type_info(llvm_ctx, res, RES_SSA_VAR(), RES_INFO(), llvm_ctx.builder.getInt32(IS_DOUBLE));
					}
					llvm_ctx.builder.CreateBr(bb_end);
					llvm_ctx.builder.SetInsertPoint(bb_ok);
					val = llvm_ctx.builder.CreateExtractValue(call, 0);
					zend_jit_save_zval_lval(llvm_ctx, op1_addr, OP1_DEF_SSA_VAR(), OP1_DEF_INFO(), val);
					if (RETURN_VALUE_USED(opline)) {
						//JIT: ZVAL_COPY_VALUE(EX_VAR(RES_OP()->var), var_ptr);
						res = zend_jit_load_slot(llvm_ctx, RES_OP()->var);
						zend_jit_save_zval_lval(llvm_ctx, res, RES_SSA_VAR(), RES_INFO(), val);
						zend_jit_save_zval_type_info(llvm_ctx, res, RES_SSA_VAR(), RES_INFO(), llvm_ctx.builder.getInt32(IS_LONG));
					}
					llvm_ctx.builder.CreateBr(bb_end);
					llvm_ctx.builder.SetInsertPoint(bb_end);
				} else {
					val = llvm_ctx.builder.CreateAdd(
						zend_jit_load_lval_c(llvm_ctx, op1_addr, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO()),
						LLVM_GET_LONG(1));
					zend_jit_save_zval_lval(llvm_ctx, op1_addr, OP1_DEF_SSA_VAR(), OP1_DEF_INFO(), val);
					if (RETURN_VALUE_USED(opline)) {
						//JIT: ZVAL_COPY_VALUE(EX_VAR(RES_OP()->var), var_ptr);
						res = zend_jit_load_slot(llvm_ctx, RES_OP()->var);
						zend_jit_save_zval_lval(llvm_ctx, res, RES_SSA_VAR(), RES_INFO(), val);
						zend_jit_save_zval_type_info(llvm_ctx, res, RES_SSA_VAR(), RES_INFO(), llvm_ctx.builder.getInt32(IS_LONG));
					}
				}
 				break;
 			case ZEND_PRE_DEC:
				//JIT: fast_decrement_function(var_ptr);
				if (OP1_DEF_MAY_BE(MAY_BE_DOUBLE)) {
					val = zend_jit_load_lval_c(llvm_ctx, op1_addr, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO());
					Function *func = Intrinsic::getDeclaration(
						llvm_ctx.module,
						Intrinsic::ssub_with_overflow,
						ArrayRef<Type*>(LLVM_GET_LONG_TY(llvm_ctx.context)));
					Value *call = llvm_ctx.builder.CreateCall2(
						func,
						val,
						LLVM_GET_LONG(1));
					BasicBlock *bb_ok = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					BasicBlock *bb_overflow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					BasicBlock *bb_end = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					zend_jit_unexpected_br(llvm_ctx,
					    llvm_ctx.builder.CreateExtractValue(call, 1),
						bb_overflow,
						bb_ok);
					llvm_ctx.builder.SetInsertPoint(bb_overflow);
					if (!op1_addr) {
						op1_addr = zend_jit_load_slot(llvm_ctx, OP1_OP()->var);
					}
#if defined(__x86_64__)
					zend_jit_save_zval_lval(llvm_ctx,
						op1_addr, OP1_DEF_SSA_VAR(), OP1_DEF_INFO(),
						LLVM_GET_LONG(0xc3e0000000000000));
#else
					zend_jit_save_zval_value(llvm_ctx,
						op1_addr,
						LLVM_GET_LONG(0x00200000),
						LLVM_GET_LONG(0xc1e00000));
#endif
					zend_jit_save_zval_type_info(llvm_ctx, op1_addr, OP1_DEF_SSA_VAR(), OP1_DEF_INFO(), llvm_ctx.builder.getInt32(IS_DOUBLE));
					if (RETURN_VALUE_USED(opline)) {
						//JIT: ZVAL_COPY_VALUE(EX_VAR(RES_OP()->var), var_ptr);
						res = zend_jit_load_slot(llvm_ctx, RES_OP()->var);
#if defined(__x86_64__)
						zend_jit_save_zval_lval(llvm_ctx,
							res, RES_SSA_VAR(), RES_INFO(),
							LLVM_GET_LONG(0x43e0000000000000));
#else
						zend_jit_save_zval_value(llvm_ctx,
							res,
							LLVM_GET_LONG(0x0),
							LLVM_GET_LONG(0x41e00000));
#endif
						zend_jit_save_zval_type_info(llvm_ctx, res, RES_SSA_VAR(), RES_INFO(), llvm_ctx.builder.getInt32(IS_DOUBLE));
					}
					llvm_ctx.builder.CreateBr(bb_end);
					llvm_ctx.builder.SetInsertPoint(bb_ok);
					val = llvm_ctx.builder.CreateExtractValue(call, 0);
					zend_jit_save_zval_lval(llvm_ctx, op1_addr, OP1_DEF_SSA_VAR(), OP1_DEF_INFO(), val);
					if (RETURN_VALUE_USED(opline)) {
						//JIT: ZVAL_COPY_VALUE(EX_VAR(RES_OP()->var), var_ptr);
						res = zend_jit_load_slot(llvm_ctx, RES_OP()->var);
						zend_jit_save_zval_lval(llvm_ctx, res, RES_SSA_VAR(), RES_INFO(), val);
						zend_jit_save_zval_type_info(llvm_ctx, res, RES_SSA_VAR(), RES_INFO(), llvm_ctx.builder.getInt32(IS_LONG));
					}
					llvm_ctx.builder.CreateBr(bb_end);
					llvm_ctx.builder.SetInsertPoint(bb_end);
				} else {
					val = llvm_ctx.builder.CreateSub(
						zend_jit_load_lval_c(llvm_ctx, op1_addr, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO()),
						LLVM_GET_LONG(1));
					zend_jit_save_zval_lval(llvm_ctx, op1_addr, OP1_DEF_SSA_VAR(), OP1_DEF_INFO(), val);
					if (RETURN_VALUE_USED(opline)) {
						//JIT: ZVAL_COPY_VALUE(EX_VAR(RES_OP()->var), var_ptr);
						res = zend_jit_load_slot(llvm_ctx, RES_OP()->var);
						zend_jit_save_zval_lval(llvm_ctx, res, RES_SSA_VAR(), RES_INFO(), val);
						zend_jit_save_zval_type_info(llvm_ctx, res, RES_SSA_VAR(), RES_INFO(), llvm_ctx.builder.getInt32(IS_LONG));
					}
				}
				break;
 			case ZEND_POST_INC:
				//JIT: ZVAL_COPY_VALUE(EX_VAR(RES_OP()->var), var_ptr);
				val = zend_jit_load_lval_c(llvm_ctx, op1_addr, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO());
				res = zend_jit_load_slot(llvm_ctx, RES_OP()->var);
				zend_jit_save_zval_lval(llvm_ctx, res, RES_SSA_VAR(), RES_INFO(), val);
				zend_jit_save_zval_type_info(llvm_ctx, res, RES_SSA_VAR(), RES_INFO(), llvm_ctx.builder.getInt32(IS_LONG));
				//JIT: fast_increment_function(var_ptr);
				if (OP1_DEF_MAY_BE(MAY_BE_DOUBLE)) {
					Function *func = Intrinsic::getDeclaration(
						llvm_ctx.module,
						Intrinsic::sadd_with_overflow,
						ArrayRef<Type*>(LLVM_GET_LONG_TY(llvm_ctx.context)));
					Value *call = llvm_ctx.builder.CreateCall2(
						func,
						val,
						LLVM_GET_LONG(1));
					BasicBlock *bb_ok = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					BasicBlock *bb_overflow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					BasicBlock *bb_end = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					zend_jit_unexpected_br(llvm_ctx,
					    llvm_ctx.builder.CreateExtractValue(call, 1),
						bb_overflow,
						bb_ok);
					llvm_ctx.builder.SetInsertPoint(bb_overflow);
					if (!op1_addr) {
						op1_addr = zend_jit_load_slot(llvm_ctx, OP1_OP()->var);
					}
#if defined(__x86_64__)
					zend_jit_save_zval_lval(llvm_ctx,
						op1_addr, OP1_DEF_SSA_VAR(), OP1_DEF_INFO(),
						LLVM_GET_LONG(0x43e0000000000000));
#else
					zend_jit_save_zval_value(llvm_ctx,
						op1_addr,
						LLVM_GET_LONG(0x0),
						LLVM_GET_LONG(0x41e00000));
#endif
					zend_jit_save_zval_type_info(llvm_ctx, op1_addr, OP1_DEF_SSA_VAR(), OP1_DEF_INFO(), llvm_ctx.builder.getInt32(IS_DOUBLE));
					llvm_ctx.builder.CreateBr(bb_end);
					llvm_ctx.builder.SetInsertPoint(bb_ok);
					val = llvm_ctx.builder.CreateExtractValue(call, 0);
					zend_jit_save_zval_lval(llvm_ctx, op1_addr, OP1_DEF_SSA_VAR(), OP1_DEF_INFO(), val);
					llvm_ctx.builder.CreateBr(bb_end);
					llvm_ctx.builder.SetInsertPoint(bb_end);
				} else {
					zend_jit_save_zval_lval(llvm_ctx, op1_addr, OP1_DEF_SSA_VAR(), OP1_DEF_INFO(),
						llvm_ctx.builder.CreateAdd(
							val,
							LLVM_GET_LONG(1)));
				}
				break;
 			case ZEND_POST_DEC:
				//JIT: ZVAL_COPY_VALUE(EX_VAR(RES_OP()->var), var_ptr);
				val = zend_jit_load_lval_c(llvm_ctx, op1_addr, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO());
				res = zend_jit_load_slot(llvm_ctx, RES_OP()->var);
				zend_jit_save_zval_lval(llvm_ctx, res, RES_SSA_VAR(), RES_INFO(), val);
				zend_jit_save_zval_type_info(llvm_ctx, res, RES_SSA_VAR(), RES_INFO(), llvm_ctx.builder.getInt32(IS_LONG));
				//JIT: fast_decrement_function(var_ptr);
				if (OP1_DEF_MAY_BE(MAY_BE_DOUBLE)) {
					Function *func = Intrinsic::getDeclaration(
						llvm_ctx.module,
						Intrinsic::ssub_with_overflow,
						ArrayRef<Type*>(LLVM_GET_LONG_TY(llvm_ctx.context)));
					Value *call = llvm_ctx.builder.CreateCall2(
						func,
						val,
						LLVM_GET_LONG(1));
					BasicBlock *bb_ok = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					BasicBlock *bb_overflow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					BasicBlock *bb_end = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					zend_jit_unexpected_br(llvm_ctx,
					    llvm_ctx.builder.CreateExtractValue(call, 1),
						bb_overflow,
						bb_ok);
					llvm_ctx.builder.SetInsertPoint(bb_overflow);
					if (!op1_addr) {
						op1_addr = zend_jit_load_slot(llvm_ctx, OP1_OP()->var);
					}
#if defined(__x86_64__)
					zend_jit_save_zval_lval(llvm_ctx,
						op1_addr, OP1_DEF_SSA_VAR(), OP1_DEF_INFO(),
						LLVM_GET_LONG(0xc3e0000000000000));
#else
					zend_jit_save_zval_value(llvm_ctx,
						op1_addr,
						LLVM_GET_LONG(0x00200000),
						LLVM_GET_LONG(0xc1e00000));
#endif
					zend_jit_save_zval_type_info(llvm_ctx, op1_addr, OP1_DEF_SSA_VAR(), OP1_DEF_INFO(), llvm_ctx.builder.getInt32(IS_DOUBLE));
					llvm_ctx.builder.CreateBr(bb_end);
					llvm_ctx.builder.SetInsertPoint(bb_ok);
					val = llvm_ctx.builder.CreateExtractValue(call, 0);
					zend_jit_save_zval_lval(llvm_ctx, op1_addr, OP1_DEF_SSA_VAR(), OP1_DEF_INFO(), val);
					llvm_ctx.builder.CreateBr(bb_end);
					llvm_ctx.builder.SetInsertPoint(bb_end);
				} else {
					zend_jit_save_zval_lval(llvm_ctx, op1_addr, OP1_DEF_SSA_VAR(), OP1_DEF_INFO(),
						llvm_ctx.builder.CreateSub(
							val,
							LLVM_GET_LONG(1)));
				}
				break;
 			default:
				ASSERT_NOT_REACHED();
		}
 		if (OP1_MAY_BE(MAY_BE_ERROR | (MAY_BE_ANY - MAY_BE_LONG))) {
			bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			llvm_ctx.builder.CreateBr(bb_finish);
 		}
 		if (bb_nolong) {
			llvm_ctx.builder.SetInsertPoint(bb_nolong);
		}
 	}

 	if (OP1_OP_TYPE() == IS_VAR && OP1_MAY_BE(MAY_BE_ERROR)) {
		BasicBlock *bb_error = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_not_error = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		zend_jit_unexpected_br(llvm_ctx,
				llvm_ctx.builder.CreateIsNull(op1_addr),
					bb_error,
					bb_not_error);
		llvm_ctx.builder.SetInsertPoint(bb_error);
		if (opline->opcode == ZEND_POST_INC
		 || opline->opcode == ZEND_POST_DEC
		 || RETURN_VALUE_USED(opline)) {
			//JIT: ZVAL_NULL(EX_VAR(RES_OP()->var));
			res = zend_jit_load_slot(llvm_ctx, RES_OP()->var);
			zend_jit_save_zval_type_info(llvm_ctx, res, RES_SSA_VAR(), RES_INFO(), llvm_ctx.builder.getInt32(IS_NULL));
		}
		if (!bb_finish) {
			bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		}
		llvm_ctx.builder.CreateBr(bb_finish);
		llvm_ctx.builder.SetInsertPoint(bb_not_error);
 	}

	if (OP1_MAY_BE(MAY_BE_ANY - MAY_BE_LONG)) {
		if (opline->opcode == ZEND_PRE_INC || opline->opcode == ZEND_PRE_DEC) {
			//JIT: ZVAL_DEREF(var_ptr);
			op1_addr = zend_jit_deref(llvm_ctx, op1_addr, OP1_SSA_VAR(), OP1_INFO());
			//JIT: SEPARATE_ZVAL_NOREF(var_ptr);
			zend_jit_separate_zval_noref(llvm_ctx, op1_addr, NULL, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO(), opline);

			if (OP1_MAY_BE(MAY_BE_ANY - (MAY_BE_LONG|MAY_BE_DOUBLE))) {
				//JIT: (in|de)crement_function(var_ptr);
				Function *_helper = zend_jit_get_helper(
						llvm_ctx,
						opline->opcode == ZEND_PRE_INC ?
							(void*)increment_function :
							(void*)decrement_function,
						opline->opcode == ZEND_PRE_INC ?
							ZEND_JIT_SYM("increment_function") :
							ZEND_JIT_SYM("decrement_function"),
						ZEND_JIT_HELPER_FAST_CALL,
						Type::getVoidTy(llvm_ctx.context),
						llvm_ctx.zval_ptr_type,
						NULL,
						NULL,
						NULL,
						NULL);

				if (OP1_INFO() & (MAY_BE_IN_REG)) {
					op1_addr = zend_jit_reload_from_reg(llvm_ctx, OP1_SSA_VAR(), OP1_INFO());
				}
				CallInst *call = llvm_ctx.builder.CreateCall(_helper, op1_addr);
				call->setCallingConv(CallingConv::X86_FastCall);
				if (OP1_DEF_INFO() & (MAY_BE_IN_REG)) {
					zend_jit_reload_to_reg(llvm_ctx,
						op1_addr ? op1_addr : zend_jit_load_slot(llvm_ctx, opline->op1.var),
						OP1_DEF_SSA_VAR(), OP1_DEF_INFO());
				}
			} else {
				Value *dval = zend_jit_load_dval(llvm_ctx, op1_addr, OP1_SSA_VAR(), OP1_INFO());
				if (opline->opcode == ZEND_PRE_INC || opline->opcode == ZEND_POST_INC) {
					dval = llvm_ctx.builder.CreateFAdd(
						dval,
						llvm_ctx.builder.CreateSIToFP(
							llvm_ctx.builder.getInt32(1),
							Type::getDoubleTy(llvm_ctx.context)));
				} else if (opline->opcode == ZEND_PRE_DEC || opline->opcode == ZEND_POST_DEC) {
					dval = llvm_ctx.builder.CreateFSub(
						dval,
						llvm_ctx.builder.CreateSIToFP(
							llvm_ctx.builder.getInt32(1),
							Type::getDoubleTy(llvm_ctx.context)));
				} else {
					ASSERT_NOT_REACHED();
				}
				zend_jit_save_zval_dval(llvm_ctx, op1_addr, OP1_SSA_VAR(), OP1_INFO(), dval);
			}

			if (RETURN_VALUE_USED(opline)) {
				//JIT: ZVAL_COPY(EX_VAR(RES_OP()->var), var_ptr);
				res = zend_jit_load_slot(llvm_ctx, RES_OP()->var);
				op1_type_info = zend_jit_load_type_info_c(llvm_ctx, op1_addr, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_DEF_INFO());
				zend_jit_copy_value(llvm_ctx, res, 0, RES_SSA_VAR(), RES_INFO(),
						op1_addr, op1_type_info, OP1_OP_TYPE(), OP1_OP(), OP1_DEF_SSA_VAR(), OP1_DEF_INFO());
				zend_jit_try_addref(llvm_ctx, res, op1_type_info, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_DEF_INFO());
			}

			//JIT: FREE_OP1_VAR_PTR();
			if (opline->op1_type == IS_VAR && should_free) {
				zend_jit_free_var_ptr(llvm_ctx, should_free, OP1_SSA_VAR(), OP1_INFO(), opline);
			}
		} else if (opline->opcode == ZEND_POST_INC || opline->opcode == ZEND_POST_DEC) {
			BasicBlock *bb_op = NULL;
			op1_type_info = zend_jit_load_type_info_c(llvm_ctx, op1_addr, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO());

			if (OP1_MAY_BE(MAY_BE_REF)) {
				BasicBlock *bb_no_ref = NULL;
				if (OP1_MAY_BE(MAY_BE_RC1|MAY_BE_RCN)) {
					BasicBlock *bb_ref = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					bb_no_ref = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					//JIT: if (UNEXPECTED(Z_ISREF_P(var_ptr))) {
					zend_jit_unexpected_br(llvm_ctx,
						llvm_ctx.builder.CreateICmpEQ(
							op1_type_info,
							llvm_ctx.builder.getInt32(IS_REFERENCE_EX)),
						bb_ref,
						bb_no_ref);
					llvm_ctx.builder.SetInsertPoint(bb_ref);
				}
				//JIT: var_ptr = Z_REFVAL_P(var_ptr);
				Value *ref = zend_jit_load_reference(llvm_ctx,
					zend_jit_load_counted(llvm_ctx, op1_addr, OP1_SSA_VAR(), OP1_INFO()));
				Value *ref_type_info = zend_jit_load_type_info_c(llvm_ctx, ref, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO());
				//JIT: ZVAL_DUP(EX_VAR(RES_OP()->var), var_ptr);
				res = zend_jit_load_slot(llvm_ctx, RES_OP()->var);
				zend_jit_copy_value(llvm_ctx, res, 0, RES_SSA_VAR(), RES_INFO(),
						ref, ref_type_info, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO());
				zend_jit_zval_copy_ctor(llvm_ctx, res, ref_type_info,
						OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO(), opline);
				if (!bb_op) {
					bb_op = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				}
				llvm_ctx.builder.CreateBr(bb_op);
				if (bb_no_ref) {
					llvm_ctx.builder.SetInsertPoint(bb_no_ref);
				}
			}

			if (OP1_MAY_BE(MAY_BE_RC1|MAY_BE_RCN)) {
				//JIT: ZVAL_COPY_VALUE(EX_VAR(RES_OP()->var), var_ptr);
				res = zend_jit_load_slot(llvm_ctx, RES_OP()->var);
				zend_jit_copy_value(llvm_ctx, res, 0, RES_SSA_VAR(), RES_INFO(),
						op1_addr, op1_type_info, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO());
				//JIT: zval_opt_copy_ctor(var_ptr);
				zend_jit_zval_copy_ctor(llvm_ctx, op1_addr, op1_type_info,
						OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO(), opline);
				if (bb_op) {
					llvm_ctx.builder.CreateBr(bb_op);
				}
			}

			if (bb_op) {
				llvm_ctx.builder.SetInsertPoint(bb_op);
			}
			
			if (OP1_MAY_BE(MAY_BE_ANY - (MAY_BE_LONG|MAY_BE_DOUBLE))) {
				//JIT: (in|de)crement_function(var_ptr);
				Function *_helper = zend_jit_get_helper(
						llvm_ctx,
						opline->opcode == ZEND_POST_INC ?
							(void*)increment_function :
							(void*)decrement_function,
						opline->opcode == ZEND_POST_INC ?
							ZEND_JIT_SYM("increment_function") :
							ZEND_JIT_SYM("decrement_function"),
						ZEND_JIT_HELPER_FAST_CALL,
						Type::getVoidTy(llvm_ctx.context),
						llvm_ctx.zval_ptr_type,
						NULL,
						NULL,
						NULL,
						NULL);

				if (OP1_INFO() & (MAY_BE_IN_REG)) {
					op1_addr = zend_jit_reload_from_reg(llvm_ctx, OP1_SSA_VAR(), OP1_INFO());
				}
				CallInst *call = llvm_ctx.builder.CreateCall(_helper, op1_addr);
				call->setCallingConv(CallingConv::X86_FastCall);
				if (OP1_DEF_INFO() & (MAY_BE_IN_REG)) {
					zend_jit_reload_to_reg(llvm_ctx,
						op1_addr ? op1_addr : zend_jit_load_slot(llvm_ctx, opline->op1.var),
						OP1_DEF_SSA_VAR(), OP1_DEF_INFO());
				}
			} else {
				Value *dval = zend_jit_load_dval(llvm_ctx, op1_addr, OP1_SSA_VAR(), OP1_INFO());
				if (opline->opcode == ZEND_PRE_INC || opline->opcode == ZEND_POST_INC) {
					dval = llvm_ctx.builder.CreateFAdd(
						dval,
						llvm_ctx.builder.CreateSIToFP(
							llvm_ctx.builder.getInt32(1),
							Type::getDoubleTy(llvm_ctx.context)));
				} else if (opline->opcode == ZEND_PRE_DEC || opline->opcode == ZEND_POST_DEC) {
					dval = llvm_ctx.builder.CreateFSub(
						dval,
						llvm_ctx.builder.CreateSIToFP(
							llvm_ctx.builder.getInt32(1),
							Type::getDoubleTy(llvm_ctx.context)));
				} else {
					ASSERT_NOT_REACHED();
				}
				zend_jit_save_zval_dval(llvm_ctx, op1_addr, OP1_SSA_VAR(), OP1_INFO(), dval);
			}

			//JIT: FREE_OP1_VAR_PTR();
			if (opline->op1_type == IS_VAR && should_free) {
				zend_jit_free_var_ptr(llvm_ctx, should_free, OP1_SSA_VAR(), OP1_INFO(), opline);
			}
		} else {
			ASSERT_NOT_REACHED();
		}
	 	if (bb_finish) {
			llvm_ctx.builder.CreateBr(bb_finish);
		}
	}

 	if (bb_finish) {
		llvm_ctx.builder.SetInsertPoint(bb_finish);
	}
	//JIT: CHECK_EXCEPTION(); ???
	//JIT: ZEND_VM_NEXT_OPCODE();

	llvm_ctx.valid_opline = 0;
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_check_arg_send_type */
static int zend_jit_check_arg_send_type(zend_llvm_ctx    &llvm_ctx,
										Value			 *call,
                                        int               arg_num,
                                        uint32_t          flags,
                                        BasicBlock       *bb_true,
                                        BasicBlock       *bb_false)
{
	Value *func;
	BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	BasicBlock *bb_check = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

	// JIT: func = EX(call)->func
	func = llvm_ctx.builder.CreateAlignedLoad(
				zend_jit_GEP(
					llvm_ctx,
					call,
					offsetof(zend_execute_data, func),
					PointerType::getUnqual(PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context)))), 4);

	// JIT: if (arg_num > func->common.num_args)
	Value *num_args = llvm_ctx.builder.CreateAlignedLoad(
				zend_jit_GEP(
					llvm_ctx,
					func,
					offsetof(zend_op_array, num_args),
					PointerType::getUnqual(Type::getInt32Ty(llvm_ctx.context))), 4);
	zend_jit_unexpected_br(llvm_ctx,
		llvm_ctx.builder.CreateICmpUGT(
			llvm_ctx.builder.getInt32(arg_num),
			num_args),
		bb_follow,
		bb_check);

	// JIT: if (EXPECTED((zf->common.fn_flags & ZEND_ACC_VARIADIC) == 0)) {
	llvm_ctx.builder.SetInsertPoint(bb_follow);
	bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	zend_jit_expected_br(llvm_ctx,
		llvm_ctx.builder.CreateICmpEQ(
			llvm_ctx.builder.CreateAnd(
				llvm_ctx.builder.CreateAlignedLoad(
					zend_jit_GEP(
						llvm_ctx,
						func,
						offsetof(zend_function, common.fn_flags),
						PointerType::getUnqual(Type::getInt32Ty(llvm_ctx.context))), 4),
				llvm_ctx.builder.getInt32(ZEND_ACC_VARIADIC)),
			llvm_ctx.builder.getInt32(0)),
		bb_false,
		bb_follow);
	// JIT: arg_num = zf->common.num_args;
	// JIT: UNEXPECTED((zf->common.arg_info[arg_num-1].pass_by_reference & mask) != 0);
	llvm_ctx.builder.SetInsertPoint(bb_follow);
	zend_jit_unexpected_br(llvm_ctx,
		llvm_ctx.builder.CreateICmpNE(
			llvm_ctx.builder.CreateAnd(
				llvm_ctx.builder.CreateAlignedLoad(
					llvm_ctx.builder.CreateInBoundsGEP(
						llvm_ctx.builder.CreateAlignedLoad(
							zend_jit_GEP(
								llvm_ctx,
								func,
								offsetof(zend_function, common.arg_info),
								PointerType::getUnqual(PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context)))), 4),
//???
						llvm_ctx.builder.CreateAdd(
							llvm_ctx.builder.CreateMul(
								num_args,
								llvm_ctx.builder.getInt32(sizeof(zend_arg_info))),
							llvm_ctx.builder.getInt32(offsetof(zend_arg_info,pass_by_reference)))), 1),
				llvm_ctx.builder.getInt8(flags)),
			llvm_ctx.builder.getInt8(0)),
		bb_true,
		bb_false);

	// JIT: UNEXPECTED((zf->common.arg_info[arg_num-1].pass_by_reference & mask) != 0);
	llvm_ctx.builder.SetInsertPoint(bb_check);
	zend_jit_unexpected_br(llvm_ctx,
		llvm_ctx.builder.CreateICmpNE(
			llvm_ctx.builder.CreateAnd(
				llvm_ctx.builder.CreateAlignedLoad(
					zend_jit_GEP(
						llvm_ctx,
						llvm_ctx.builder.CreateAlignedLoad(
							zend_jit_GEP(
								llvm_ctx,
								func,
								offsetof(zend_function, common.arg_info),
								PointerType::getUnqual(PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context)))), 4),
						((arg_num-1) * sizeof(zend_arg_info) + offsetof(zend_arg_info,pass_by_reference)),
						PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context))), 1),
				llvm_ctx.builder.getInt8(flags)),
			llvm_ctx.builder.getInt8(0)),
		bb_true,
		bb_false);

	return 1;
}
/* }}} */

/* {{{ static int zend_jit_send_val */
static int zend_jit_send_val(zend_llvm_ctx    &llvm_ctx,
                             zend_op_array    *op_array,
                             zend_op          *opline,
                             zend_bool         check_ref)
{
	Value *call = llvm_ctx.builder.CreateAlignedLoad(
					zend_jit_GEP(
						llvm_ctx,
						llvm_ctx._execute_data,
						offsetof(zend_execute_data, call),
						PointerType::getUnqual(PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context)))), 4);
	if (check_ref) {
		//JIT: if (ARG_MUST_BE_SENT_BY_REF(EX(call)->func, OP2_OP()->num))
		BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_error = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

		zend_jit_check_arg_send_type(
			llvm_ctx,
			call,
			OP2_OP()->num,
			ZEND_SEND_BY_REF,
			bb_error,
			bb_follow);
		                             
		//JIT: zend_error_noreturn(E_ERROR, "Cannot pass parameter %d by reference", OP2_OP()->num);
		llvm_ctx.builder.SetInsertPoint(bb_error);
		//JIT: SAVE_OPLINE()
		if (!llvm_ctx.valid_opline) {
			JIT_CHECK(zend_jit_store_opline(llvm_ctx, opline, false));
		}
		//JIT: zend_throw_error(NULL, "Cannot pass parameter %d by reference", opline->op2.num);
		zend_jit_throw_error(llvm_ctx, opline, NULL, "Cannot pass parameter %d by reference", llvm_ctx.builder.getInt32(OP2_OP()->num));
		//JIT: FREE_UNFETCHED_OP1();
		if (opline->op1_type & (IS_VAR|IS_TMP_VAR)) {
			Value *op1_addr = zend_jit_load_operand(llvm_ctx,
				OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO(), 0, opline);
			zend_jit_zval_ptr_dtor_ex(llvm_ctx, op1_addr, NULL, OP1_SSA_VAR(), OP1_INFO(), opline->lineno, 0);
		}

		//JIT: arg = ZEND_CALL_VAR(EX(call), opline->result.var);
		Value *arg_addr = zend_jit_GEP(
						llvm_ctx,
						call,
						sizeof(zval) * (OP2_OP()->num + ZEND_CALL_FRAME_SLOT - 1),
						llvm_ctx.zval_ptr_type);
		//JIT: ZVAL_UNDEF(arg);
		zend_jit_save_zval_type_info(llvm_ctx, arg_addr, -1, MAY_BE_ANY, llvm_ctx.builder.getInt32(IS_UNDEF));
		//JIT: HANDLE_EXCEPTION();
		llvm_ctx.builder.CreateBr(zend_jit_find_exception_bb(llvm_ctx, opline));
		llvm_ctx.builder.SetInsertPoint(bb_follow);
//???	} else if (zend_jit_pass_unused_arg(llvm_ctx, op_array, opline)) {
//???		return 1;
	}

	//JIT: value = GET_OP1_ZVAL_PTR(BP_VAR_R);
	Value *op1_addr = zend_jit_load_operand(llvm_ctx,
				OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO(), 0, opline);
	//JIT: arg = ZEND_CALL_ARG(EX(call), OP2_OP()->num);
	Value *arg_addr = zend_jit_GEP(
						llvm_ctx,
						call,
						sizeof(zval) * (OP2_OP()->num + ZEND_CALL_FRAME_SLOT - 1),
						llvm_ctx.zval_ptr_type);

	//JIT: ZVAL_COPY_VALUE(arg, value);
	zend_jit_copy_value(llvm_ctx, arg_addr, 0, -1, MAY_BE_ANY,
		op1_addr, NULL, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO());
	if (OP1_OP_TYPE() == IS_CONST) {
		if (UNEXPECTED(Z_OPT_COPYABLE_P(RT_CONSTANT(llvm_ctx.op_array, *OP1_OP())))) {
			//JIT: zend_copy_ctor_func(arg) 
			zend_jit_copy_ctor_func(llvm_ctx, arg_addr, opline->lineno);
		}
	}
	//JIT: ZEND_VM_NEXT_OPCODE();
	llvm_ctx.valid_opline = 0;
	return 1;
}
/* }}} */

/* {{{ static Value* zend_jit_new_ref */
static Value* zend_jit_new_ref_ex(zend_llvm_ctx    &llvm_ctx,
                                  Value            *ref,
                                  int               ref_ssa_var,
                                  uint32_t          ref_info,
                                  uint32_t          refcount,
                                  Value            *val,
                                  Value            *val_type_info,
                                  zend_uchar        val_op_type,
                                  znode_op         *val_op,
                                  int               val_ssa_var,
                                  uint32_t          val_info,
                                  uint32_t          lineno)
{
	//JIT: zend_reference *_ref = emalloc(sizeof(zend_reference));
	Value *reference = zend_jit_emalloc(llvm_ctx,
		LLVM_GET_LONG(sizeof(zend_reference)), lineno);

	//JIT: GC_REFCOUNT(_ref) = refcount;
	llvm_ctx.builder.CreateAlignedStore(
		llvm_ctx.builder.getInt32(refcount),
		zend_jit_refcount_addr(llvm_ctx, reference), 4);

	//JIT: GC_TYPE_INFO(_ref) = IS_REFERENCE;		
	llvm_ctx.builder.CreateAlignedStore(
		llvm_ctx.builder.getInt32(IS_REFERENCE),
		zend_jit_GEP(
			llvm_ctx,
			reference,
			offsetof(zend_refcounted, gc.u.type_info),
			PointerType::getUnqual(Type::getInt32Ty(llvm_ctx.context))), 4);
	
	//JIT: ZVAL_COPY_VALUE(&_ref->val, r);
	zend_jit_copy_value(llvm_ctx,
		zend_jit_load_reference(llvm_ctx, reference), MAY_BE_ANY, -1, MAY_BE_ANY,
		val, val_type_info, val_op_type, val_op, val_ssa_var, val_info);
	
	//JIT: Z_REF_P(z) = _ref;
	zend_jit_save_zval_ptr(llvm_ctx, ref, ref_ssa_var, ref_info,
		reference);
	
	//JIT: Z_TYPE_INFO_P(z) = IS_REFERENCE_EX;
	zend_jit_save_zval_type_info(llvm_ctx, ref, ref_ssa_var, ref_info,
		llvm_ctx.builder.getInt32(IS_REFERENCE_EX));
	
	return reference;
}
/* }}} */

/* {{{ static int zend_jit_send_ref */
static int zend_jit_send_ref(zend_llvm_ctx    &llvm_ctx,
                             zend_op_array    *op_array,
                             zend_op          *opline)
{
	Value *should_free = NULL;
	Value *op1_addr = NULL;
	Value *op1_type_info = NULL;
	BasicBlock *bb_finish = NULL;

	//JIT: varptr = GET_OP1_ZVAL_PTR_PTR(BP_VAR_W);
	op1_addr = zend_jit_load_operand_addr(llvm_ctx,
				OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO(), 0, opline, 0, BP_VAR_W,
				&should_free);

	if (OP1_MAY_BE(MAY_BE_IN_REG)) {
		op1_addr = zend_jit_reload_from_reg(llvm_ctx, OP1_SSA_VAR(), OP1_INFO());
	}

	if (opline->op1_type == IS_VAR) {
		BasicBlock *bb_error = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		//JIT: if (UNEXPECTED(varptr == NULL)) {
		zend_jit_unexpected_br(llvm_ctx,
				llvm_ctx.builder.CreateIsNull(op1_addr),
				bb_error,
				bb_follow);
		llvm_ctx.builder.SetInsertPoint(bb_error);
		//JIT: zend_error_noreturn(E_ERROR, "Only variables can be passed by reference");
		zend_jit_error_noreturn(llvm_ctx, opline, E_ERROR,
				"Only variables can be passed by reference");
		llvm_ctx.builder.SetInsertPoint(bb_follow);
	}

	Value *call = llvm_ctx.builder.CreateAlignedLoad(
					zend_jit_GEP(
						llvm_ctx,
						llvm_ctx._execute_data,
						offsetof(zend_execute_data, call),
						PointerType::getUnqual(PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context)))), 4);
	//JIT: arg = ZEND_CALL_ARG(EX(call), opline->op2.num);
	Value *arg_addr = zend_jit_GEP(
						llvm_ctx,
						call,
						sizeof(zval) * (OP2_OP()->num + ZEND_CALL_FRAME_SLOT - 1),
						llvm_ctx.zval_ptr_type);

	if (opline->op1_type == IS_VAR && OP1_MAY_BE(MAY_BE_ERROR)) {
		BasicBlock *bb_error = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		//JIT: if (UNEXPECTED(varptr == &EG(error_zval))) {
		zend_jit_unexpected_br(llvm_ctx,
			llvm_ctx.builder.CreateICmpEQ(
				op1_addr,
				llvm_ctx._EG_error_zval),
				bb_error,
				bb_follow);
		llvm_ctx.builder.SetInsertPoint(bb_error);
		//JIT: ZVAL_NEW_REF(arg, &EG(uninitialized_zval));
		zend_jit_new_ref_ex(llvm_ctx, arg_addr, -1, MAY_BE_ANY, 1,
			NULL, llvm_ctx.builder.getInt32(IS_NULL), OP1_OP_TYPE(), OP1_OP(), -1, MAY_BE_NULL, opline->lineno);
		if (!bb_finish) {
			bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		}
		llvm_ctx.builder.CreateBr(bb_finish);
		llvm_ctx.builder.SetInsertPoint(bb_follow);
	}

	BasicBlock *bb_noref = NULL;
	if (OP1_MAY_BE(MAY_BE_REF)) {
		if (OP1_MAY_BE(MAY_BE_RC1|MAY_BE_RCN)) {
			//JIT: if (Z_ISREF_P(varptr)) {
			op1_type_info = zend_jit_load_type_info(llvm_ctx, op1_addr, OP1_SSA_VAR(), OP1_INFO());
			BasicBlock *bb_ref = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_noref = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			zend_jit_unexpected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(
					op1_type_info,
					llvm_ctx.builder.getInt32(IS_REFERENCE_EX)),
				bb_ref,
				bb_noref);
			llvm_ctx.builder.SetInsertPoint(bb_ref);
		}
		//JIT: Z_ADDREF_P(varptr);
		zend_jit_addref(llvm_ctx, 
			zend_jit_load_counted(llvm_ctx, op1_addr, OP1_SSA_VAR(), OP1_INFO()));
		//JIT: ZVAL_COPY_VALUE(arg, varptr);
		zend_jit_copy_value(llvm_ctx,
			arg_addr, MAY_BE_ANY, -1, MAY_BE_ANY,
			op1_addr, op1_type_info, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO());
		if (bb_noref) {
			if (!bb_finish) {
				bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(bb_finish);
		}
	}
	if (OP1_MAY_BE(MAY_BE_RC1|MAY_BE_RCN)) {
		if (bb_noref) {
			llvm_ctx.builder.SetInsertPoint(bb_noref);
		}
		if (opline->op1_type == IS_VAR) {
			//JIT: if (UNEXPECTED(Z_TYPE_P(EX_VAR(opline->op1.var)) != IS_INDIRECT)) {
			BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			BasicBlock *bb_next = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			zend_jit_unexpected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpNE(
					zend_jit_load_type_info(llvm_ctx,
						zend_jit_load_slot(llvm_ctx, opline->op1.var),
						-1, MAY_BE_ANY),
					llvm_ctx.builder.getInt32(IS_INDIRECT)),
				bb_follow,
				bb_next);
			llvm_ctx.builder.SetInsertPoint(bb_follow);

			//JIT: ZVAL_NEW_REF(arg, varptr);
			zend_jit_new_ref_ex(llvm_ctx, arg_addr, -1, MAY_BE_ANY, 1,
				op1_addr, op1_type_info, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO(), opline->lineno);
			if (!bb_finish) {
				bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(bb_finish);
			llvm_ctx.builder.SetInsertPoint(bb_next);
		}
		//JIT: ZVAL_NEW_REF(arg, varptr);
		Value *counted = zend_jit_new_ref_ex(llvm_ctx, arg_addr, -1, MAY_BE_ANY, 2,
			op1_addr, op1_type_info, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO(), opline->lineno);
		//JIT: ZVAL_REF(varptr, Z_REF_P(arg));
		zend_jit_save_zval_ptr(llvm_ctx, op1_addr,
			(opline->op1_type == IS_CV) ? OP1_DEF_SSA_VAR() : -1,
			(opline->op1_type == IS_CV) ? OP1_DEF_INFO() : MAY_BE_ANY,
			counted);
		zend_jit_save_zval_type_info(llvm_ctx, op1_addr,
			(opline->op1_type == IS_CV) ? OP1_DEF_SSA_VAR() : -1,
			(opline->op1_type == IS_CV) ? OP1_DEF_INFO() : MAY_BE_ANY,
			llvm_ctx.builder.getInt32(IS_REFERENCE_EX));
		if (bb_finish) {
			llvm_ctx.builder.CreateBr(bb_finish);
		}
	}

	if (bb_finish) {
		llvm_ctx.builder.SetInsertPoint(bb_finish);
	}
	
	//JIT: FREE_OP1_VAR_PTR();
	if (opline->op1_type == IS_VAR && should_free) {
		zend_jit_free_var_ptr(llvm_ctx, should_free, OP1_SSA_VAR(), OP1_INFO(), opline);
	}

	//JIT: ZEND_VM_NEXT_OPCODE();
	llvm_ctx.valid_opline = 0;
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_send_var */
static int zend_jit_send_var(zend_llvm_ctx    &llvm_ctx,
                             zend_op_array    *op_array,
                             zend_op          *opline,
                             zend_bool         check_ref)
{
	BasicBlock *bb_finish = NULL;
	Value *call = llvm_ctx.builder.CreateAlignedLoad(
					zend_jit_GEP(
						llvm_ctx,
						llvm_ctx._execute_data,
						offsetof(zend_execute_data, call),
						PointerType::getUnqual(PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context)))), 4);
	if (check_ref) {
		//JIT: if (ARG_SHOULD_BE_SENT_BY_REF(EX(call)->func, OP2_OP()->num)) {
		BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_ref = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

		zend_jit_check_arg_send_type(
			llvm_ctx,
			call,
			OP2_OP()->num,
			ZEND_SEND_BY_REF|ZEND_SEND_PREFER_REF,
			bb_ref,
			bb_follow);
		                             
		llvm_ctx.builder.SetInsertPoint(bb_ref);
		zend_jit_send_ref(llvm_ctx, op_array, opline);
		llvm_ctx.builder.CreateBr(bb_finish);

		llvm_ctx.builder.SetInsertPoint(bb_follow);

//???	} else if (zend_jit_pass_unused_arg(llvm_ctx, op_array, opline)) {
//???		return 1;
	}

	//JIT: value = GET_OP1_ZVAL_PTR(BP_VAR_R);
	Value *op1_addr = zend_jit_load_operand(llvm_ctx,
				OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO(), 0, opline);
	if (OP1_MAY_BE(MAY_BE_IN_REG)) {
		op1_addr = zend_jit_reload_from_reg(llvm_ctx, OP1_SSA_VAR(), OP1_INFO());
	}

	//JIT: arg = ZEND_CALL_ARG(EX(call), OP2_OP()->num);
	Value *arg_addr = zend_jit_GEP(
						llvm_ctx,
						call,
						sizeof(zval) * (OP2_OP()->num + ZEND_CALL_FRAME_SLOT - 1),
						llvm_ctx.zval_ptr_type);

	Value *op1_type_info = NULL;
	if (OP1_MAY_BE(MAY_BE_REF)) {
		BasicBlock *bb_noref = NULL;
		if (OP1_MAY_BE(MAY_BE_RC1|MAY_BE_RCN)) {
			//JIT: if ((OP1_TYPE == IS_CV || OP1_TYPE == IS_VAR) && Z_ISREF_P(varptr)) {
			BasicBlock *bb_ref = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_noref = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			op1_type_info = zend_jit_load_type_info_c(llvm_ctx, op1_addr, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO());
			zend_jit_unexpected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(
					op1_type_info,
					llvm_ctx.builder.getInt32(IS_REFERENCE_EX)),
				bb_ref,
				bb_noref);
			llvm_ctx.builder.SetInsertPoint(bb_ref);
		}
		//JIT: ZVAL_COPY(arg, Z_REFVAL_P(varptr));
		Value *ref = zend_jit_load_reference(llvm_ctx,
			zend_jit_load_counted(llvm_ctx, op1_addr, OP1_SSA_VAR(), OP1_INFO()));
		Value *ref_type_info = zend_jit_load_type_info_c(llvm_ctx, ref, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO());
		zend_jit_copy_value(llvm_ctx, arg_addr, 0, -1, MAY_BE_ANY,
			ref, ref_type_info, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO());
		zend_jit_try_addref(llvm_ctx, ref, ref_type_info, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO());

		//JIT: FREE_OP1(); (TODO: op1_addr is a reference, type ???)
		if (!zend_jit_free_operand(llvm_ctx, OP1_OP_TYPE(), op1_addr, NULL, OP1_SSA_VAR(), OP1_INFO(), opline->lineno)) {
		   	return 0;
		}

		if (!bb_finish) {
			bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		}
		llvm_ctx.builder.CreateBr(bb_finish);
		if (bb_noref) {
			llvm_ctx.builder.SetInsertPoint(bb_noref);
		}
	}

	if (OP1_MAY_BE(MAY_BE_RC1|MAY_BE_RCN)) {
		//JIT: ZVAL_COPY_VALUE(arg, value);
		zend_jit_copy_value(llvm_ctx, arg_addr, 0, -1, MAY_BE_ANY,
			op1_addr, op1_type_info, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO());
		if (OP1_OP_TYPE() == IS_CV) {
			//JIT: if (Z_OPT_REFCOUNTED_P(arg)) Z_ADDREF_P(arg);
			zend_jit_try_addref(llvm_ctx, op1_addr, op1_type_info, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO());
		}
		if (bb_finish) {
			llvm_ctx.builder.CreateBr(bb_finish);
		}
	}
	if (bb_finish) {
		llvm_ctx.builder.SetInsertPoint(bb_finish);
	}

	//JIT: ZEND_VM_NEXT_OPCODE();
	llvm_ctx.valid_opline = 0;
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_send_var_no_ref */
static int zend_jit_send_var_no_ref(zend_llvm_ctx    &llvm_ctx,
                                    zend_op_array    *op_array,
                                    zend_op          *opline)
{
	BasicBlock *bb_finish = NULL;
	Value *call = llvm_ctx.builder.CreateAlignedLoad(
					zend_jit_GEP(
						llvm_ctx,
						llvm_ctx._execute_data,
						offsetof(zend_execute_data, call),
						PointerType::getUnqual(PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context)))), 4);

	if (!(opline->extended_value & ZEND_ARG_COMPILE_TIME_BOUND)) {
		//JIT: if (ARG_SHOULD_BE_SENT_BY_REF(EX(call)->func, OP2_OP()->num)) {
		BasicBlock *bb_no_ref = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_ref = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

		zend_jit_check_arg_send_type(
			llvm_ctx,
			call,
			OP2_OP()->num,
			ZEND_SEND_BY_REF|ZEND_SEND_PREFER_REF,
			bb_ref,
			bb_no_ref);
		                             
		llvm_ctx.builder.SetInsertPoint(bb_no_ref);
		zend_jit_send_var(llvm_ctx, op_array, opline, 0);
		llvm_ctx.builder.CreateBr(bb_finish);

		llvm_ctx.builder.SetInsertPoint(bb_ref);
	}

	//JIT: varptr = GET_OP1_ZVAL_PTR(BP_VAR_R);
	Value *op1_addr = zend_jit_load_operand(llvm_ctx,
				OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO(), 0, opline);
	Value *op1_type_info = NULL;

	BasicBlock *bb_common = NULL;

	if (OP1_MAY_BE(MAY_BE_REF|MAY_BE_OBJECT)) {
		BasicBlock *bb_error = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_next;

		bb_common = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);;
		if (opline->extended_value & ZEND_ARG_SEND_FUNCTION) {
			BasicBlock *bb_next = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			//JIT: (Z_VAR_FLAGS_P(varptr) & IS_VAR_RET_REF)) &&
			zend_jit_expected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpNE(
					llvm_ctx.builder.CreateAnd(
						zend_jit_load_var_flags(llvm_ctx, op1_addr),
						llvm_ctx.builder.getInt32(IS_VAR_RET_REF)),
				llvm_ctx.builder.getInt32(0)),
				bb_next,
				bb_error);
			llvm_ctx.builder.SetInsertPoint(bb_next);
		}
		//JIT: (Z_ISREF_P(varptr) || Z_TYPE_P(varptr) == IS_OBJECT)) {
		op1_type_info = zend_jit_load_type_info_c(llvm_ctx, op1_addr, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO());
		bb_next = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		zend_jit_expected_br(llvm_ctx,
			llvm_ctx.builder.CreateICmpEQ(
				op1_type_info,
				llvm_ctx.builder.getInt32(IS_REFERENCE_EX)),
				bb_follow,
				bb_next);
		llvm_ctx.builder.SetInsertPoint(bb_next);
		zend_jit_expected_br(llvm_ctx,
			llvm_ctx.builder.CreateICmpEQ(
				op1_type_info,
				llvm_ctx.builder.getInt32(IS_OBJECT_EX)),
				bb_follow,
				bb_error);
		llvm_ctx.builder.SetInsertPoint(bb_follow);

		//JIT: ZVAL_MAKE_REF(varptr);
		zend_jit_make_ref(llvm_ctx, op1_addr, op1_type_info, OP1_SSA_VAR(), OP1_INFO());

		llvm_ctx.builder.CreateBr(bb_common);
		llvm_ctx.builder.SetInsertPoint(bb_error);
	}

	if (opline->extended_value & ZEND_ARG_COMPILE_TIME_BOUND) {
		if (!(opline->extended_value & ZEND_ARG_SEND_SILENT)) {
			//JIT: zend_error(E_NOTICE, "Only variables should be passed by reference");
			zend_jit_error(llvm_ctx, opline, E_NOTICE, "Only variables should be passed by reference");
		}
	} else {
		//JIT: !ARG_MAY_BE_SENT_BY_REF(EX(call)->func, opline->op2.num)) {
		BasicBlock *bb_no_ref = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_ref = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

		zend_jit_check_arg_send_type(
			llvm_ctx,
			call,
			OP2_OP()->num,
			ZEND_SEND_PREFER_REF,
			bb_ref,
			bb_no_ref);

		llvm_ctx.builder.SetInsertPoint(bb_no_ref);
		//JIT: zend_error(E_NOTICE, "Only variables should be passed by reference");
		zend_jit_error(llvm_ctx, opline, E_NOTICE, "Only variables should be passed by reference");
		llvm_ctx.builder.CreateBr(bb_ref);
		llvm_ctx.builder.SetInsertPoint(bb_ref);
	}

	if (bb_common) {
		llvm_ctx.builder.CreateBr(bb_common);
		llvm_ctx.builder.SetInsertPoint(bb_common);
	}

	//JIT: arg = ZEND_CALL_ARG(EX(call), opline->op2.num);
	Value *arg_addr = zend_jit_GEP(
			llvm_ctx,
			call,
			sizeof(zval) * (OP2_OP()->num + ZEND_CALL_FRAME_SLOT - 1),
			llvm_ctx.zval_ptr_type);

	//JIT: ZVAL_COPY_VALUE(arg, varptr);
	zend_jit_copy_value(llvm_ctx, arg_addr, 0, -1, MAY_BE_ANY,
		op1_addr, /*???op1_type_info*/NULL, OP1_OP_TYPE(), OP1_OP(),
		OP1_SSA_VAR(),
		OP1_MAY_BE(MAY_BE_OBJECT) ? MAY_BE_ANY : OP1_INFO());
	
	//JIT: CHECK_EXCEPTION();
	JIT_CHECK(zend_jit_check_exception(llvm_ctx, opline));

	if (bb_finish) {
		llvm_ctx.builder.CreateBr(bb_finish);
		llvm_ctx.builder.SetInsertPoint(bb_finish);
	}

	//JIT: ZEND_VM_NEXT_OPCODE();
	llvm_ctx.valid_opline = 0;
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_fetch_dim_r */
static int zend_jit_fetch_dim_r(zend_llvm_ctx     &llvm_ctx,
                                zend_jit_context  *ctx,
                                zend_op_array     *op_array,
                                zend_op           *opline,
                                zend_uchar         fetch_type)
{
	Value *var_ptr = NULL;
	Value *dim_ptr = NULL;
	Value *result = NULL;
	zend_bool may_threw = 0;

	var_ptr = zend_jit_load_operand(llvm_ctx,
				OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO(), 0, opline, 0, fetch_type);
	dim_ptr = zend_jit_load_operand(llvm_ctx,
				OP2_OP_TYPE(), OP2_OP(), OP2_SSA_VAR(), OP2_INFO(), 0, opline);

	result = zend_jit_load_slot(llvm_ctx, RES_OP()->var);

	Value *ret = zend_jit_fetch_dimension_address_read(
			llvm_ctx,
			zend_jit_load_operand_scope(llvm_ctx, OP1_SSA_VAR(), OP1_OP_TYPE(), ctx, op_array),
			var_ptr,
			OP1_SSA_VAR(),
			OP1_INFO(),
			dim_ptr,
			OP2_SSA_VAR(),
			OP2_INFO(),
			OP2_OP(),
			OP2_OP_TYPE(),
			fetch_type,
			opline, 
			&may_threw);

	zend_jit_copy_value(
			llvm_ctx, 
			result,
			0,
			RES_SSA_VAR(),
			RES_INFO(),
			ret,
			NULL,
			IS_VAR,
			NULL,
			-1, 
			RES_INFO() & MAY_BE_ANY);

	if (!zend_jit_free_operand(llvm_ctx, OP2_OP_TYPE(), dim_ptr, NULL, OP2_SSA_VAR(), OP2_INFO(), opline->lineno)) {
		return 0;
	}

	if (!zend_jit_free_operand(llvm_ctx, OP1_OP_TYPE(), var_ptr, NULL, OP1_SSA_VAR(), OP1_INFO(), opline->lineno)) {
		return 0;
	}

	if (may_threw) {
		JIT_CHECK(zend_jit_check_exception(llvm_ctx, opline));
	}

	llvm_ctx.valid_opline = 0;
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_fetch_dim */
static int zend_jit_fetch_dim(zend_llvm_ctx     &llvm_ctx,
                              zend_jit_context  *ctx,
                              zend_op_array     *op_array,
                              zend_op           *opline,
							  zend_uchar         fetch_type)
{
	Value *ret;
	Value *object_property;
	Value *should_free = NULL;
	Value *op1_addr = NULL;
	Value *op2_addr = NULL;
	Value *var_ptr = NULL;
	Value *dim_ptr = NULL;
	Value *result = NULL;
	Value *new_elem = NULL;
	BasicBlock *bb_ind = NULL;
	BasicBlock *bb_finish = NULL;
	BasicBlock *bb_string_offset = NULL;
	BasicBlock *bb_error = NULL;
	BasicBlock *bb_new_element = NULL;
	BasicBlock *bb_object_property = NULL;
	BasicBlock *bb_uninitialized = NULL;
	zend_bool may_threw = 0;
	PHI_DCL(val, 5);

	op1_addr = zend_jit_load_operand_addr(llvm_ctx,
			OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO(), 0, opline, 0, fetch_type,
	 		&should_free);

	if (OP1_MAY_BE(MAY_BE_IN_REG)) {
		op1_addr = zend_jit_reload_from_reg(llvm_ctx, OP1_SSA_VAR(), OP1_INFO());
	}

	if (opline->op1_type == IS_VAR) {
		BasicBlock *bb_error = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		//JIT: if (UNEXPECTED(object_ptr == NULL)) {
		zend_jit_unexpected_br(llvm_ctx,
				llvm_ctx.builder.CreateIsNull(op1_addr),
				bb_error,
				bb_follow);
		llvm_ctx.builder.SetInsertPoint(bb_error);
		//JIT: zend_error_noreturn(E_ERROR, "Cannot use string offset as an array");
		zend_jit_error_noreturn(llvm_ctx, opline, E_ERROR,
				"Cannot use string offset as an array");
		llvm_ctx.builder.SetInsertPoint(bb_follow);
	}

	var_ptr = zend_jit_deref(llvm_ctx, op1_addr, OP1_SSA_VAR(), OP1_INFO());

	if (OP2_OP_TYPE() != IS_UNUSED) {
		op2_addr = zend_jit_load_operand(llvm_ctx, OP2_OP_TYPE(), OP2_OP(), OP2_SSA_VAR(), OP2_INFO(), 0, opline);
		dim_ptr = zend_jit_deref(llvm_ctx, op2_addr, OP2_SSA_VAR(), OP2_INFO());
	}

	result = zend_jit_load_slot(llvm_ctx, RES_OP()->var);
	ret = zend_jit_fetch_dimension_address(
			llvm_ctx,
			zend_jit_load_operand_scope(llvm_ctx, OP1_SSA_VAR(), OP1_OP_TYPE(), ctx, op_array),
			result,
			var_ptr,
			NULL,
			OP1_SSA_VAR(),
			OP1_INFO(),
			OP1_OP(),
			OP1_OP_TYPE(),
			dim_ptr,
			OP2_SSA_VAR(),
			OP2_INFO(),
			OP2_OP(),
			OP2_OP_TYPE(),
			fetch_type,
			NULL,
			&bb_string_offset,
			&new_elem,
			&bb_new_element,
			&object_property,
			&bb_object_property,
			&bb_uninitialized,
			&bb_error,
			0,
			opline, 
			&may_threw);

	if (ret) {
		if (opline->extended_value != 0) {
			zend_jit_make_ref(llvm_ctx, ret, NULL, -1, MAY_BE_ANY);
		}

		PHI_ADD(val, ret);

		if (!bb_ind) {
			bb_ind = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		}
		llvm_ctx.builder.CreateBr(bb_ind);
	}

	if (bb_object_property) {

		llvm_ctx.builder.SetInsertPoint(bb_object_property);
		zend_jit_copy_value(
				llvm_ctx, 
				result,
				0,
				RES_SSA_VAR(),
				RES_INFO(),
				object_property,
				NULL,
				IS_VAR,
				NULL,
				-1, 
				RES_INFO() & MAY_BE_ANY);
		if (!bb_finish) {
			bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		}
		llvm_ctx.builder.CreateBr(bb_finish);
	}

	if (bb_string_offset) {
		llvm_ctx.builder.SetInsertPoint(bb_string_offset);
		PHI_ADD(val, llvm_ctx.builder.CreateIntToPtr(LLVM_GET_LONG(0), llvm_ctx.zval_ptr_type));
		if (!bb_ind) {
			bb_ind = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		}
		llvm_ctx.builder.CreateBr(bb_ind);
	}

	if (bb_new_element) {
		llvm_ctx.builder.SetInsertPoint(bb_new_element);
		if (opline->extended_value != 0) {
			zend_jit_make_ref(llvm_ctx, new_elem, NULL, -1, MAY_BE_NULL);
		}
		PHI_ADD(val, new_elem);
		if (!bb_ind) {
			bb_ind = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		}
		llvm_ctx.builder.CreateBr(bb_ind);
	}

	if (bb_error) {
		llvm_ctx.builder.SetInsertPoint(bb_error);
		PHI_ADD(val, llvm_ctx._EG_error_zval);
		if (!bb_ind) {
			bb_ind = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		}
		llvm_ctx.builder.CreateBr(bb_ind);
	}

	if (bb_uninitialized) {
		llvm_ctx.builder.SetInsertPoint(bb_uninitialized);
		zend_jit_save_zval_type_info(llvm_ctx, result, RES_SSA_VAR(), RES_INFO(), llvm_ctx.builder.getInt32(IS_NULL));
		if (!bb_finish) {
			bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		}
		llvm_ctx.builder.CreateBr(bb_finish);
	}

	llvm_ctx.builder.SetInsertPoint(bb_ind);
	PHI_SET(val, ret, llvm_ctx.zval_ptr_type);
	zend_jit_save_zval_ind(llvm_ctx, result, ret);
	zend_jit_save_zval_type_info(llvm_ctx, result, RES_SSA_VAR(), RES_INFO(), llvm_ctx.builder.getInt32(IS_INDIRECT));

	if (bb_finish) {
		llvm_ctx.builder.CreateBr(bb_finish);
		llvm_ctx.builder.SetInsertPoint(bb_finish);
	}

	if (!zend_jit_free_operand(llvm_ctx, OP2_OP_TYPE(), op2_addr, NULL, OP2_SSA_VAR(), OP2_INFO(), opline->lineno)) {
		return 0;
	}

	if (opline->op1_type == IS_VAR && should_free) {
		//JIT: if (READY_TO_DESTROY(free_op1)) {
		BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_skip = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		zend_jit_unexpected_br(llvm_ctx,
			llvm_ctx.builder.CreateIsNull(should_free),
			bb_skip,
			bb_follow);
		llvm_ctx.builder.SetInsertPoint(bb_follow);
		bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		Value *op1_type_info = zend_jit_load_type_info(llvm_ctx, should_free, -1, MAY_BE_ANY);
		zend_jit_unexpected_br(llvm_ctx,
			llvm_ctx.builder.CreateICmpEQ(
				llvm_ctx.builder.CreateAnd(
					op1_type_info,
					llvm_ctx.builder.getInt32(IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT)),
				llvm_ctx.builder.getInt32(0)),
				bb_skip,
				bb_follow);
		llvm_ctx.builder.SetInsertPoint(bb_follow);
		bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		Value *counted = zend_jit_load_counted(llvm_ctx, should_free, -1, MAY_BE_ANY);
		zend_jit_expected_br(llvm_ctx,				
			llvm_ctx.builder.CreateICmpEQ(
				llvm_ctx.builder.CreateAlignedLoad(
					zend_jit_refcount_addr(llvm_ctx, counted), 4),
				llvm_ctx.builder.getInt32(1)),
			bb_follow,
			bb_skip);
		llvm_ctx.builder.SetInsertPoint(bb_follow);
		//JIT: EXTRACT_ZVAL_PTR(EX_VAR(opline->result.var));
		Value *res_type_info = zend_jit_load_type_info(llvm_ctx, result, -1, MAY_BE_ANY);
		bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		zend_jit_expected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(
					res_type_info,
					llvm_ctx.builder.getInt32(IS_INDIRECT)),
				bb_follow,
				bb_skip);
		llvm_ctx.builder.SetInsertPoint(bb_follow);
		Value *tmp = zend_jit_load_ind(llvm_ctx, result);
		zend_jit_copy_value(llvm_ctx, result, 0, -1, MAY_BE_ANY,
			tmp, NULL, opline->result_type, RES_OP(), -1, MAY_BE_ANY);
		zend_jit_try_addref(llvm_ctx, result, NULL, opline->result_type, RES_OP(), -1, MAY_BE_ANY);
		llvm_ctx.builder.CreateBr(bb_skip);
		llvm_ctx.builder.SetInsertPoint(bb_skip);
	}

	if  (opline->op1_type == IS_CV) {
		zend_jit_update_reg_value(
			llvm_ctx,
			opline->op1.var,
			op1_addr,
			OP1_SSA_VAR(),
			OP1_INFO(),
			OP1_DEF_SSA_VAR(),
			OP1_DEF_INFO());
	} else if (opline->op1_type == IS_VAR && should_free) {
		zend_jit_free_var_ptr(llvm_ctx, should_free, OP1_SSA_VAR(), OP1_INFO(), opline);
	}

	if (may_threw) {
		JIT_CHECK(zend_jit_check_exception(llvm_ctx, opline));
	}

	llvm_ctx.valid_opline = 0;
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_strlen */
static int zend_jit_strlen(zend_llvm_ctx  &llvm_ctx,
                           zend_op_array  *op_array,
                           zend_op        *opline)
{
	Value *result = zend_jit_load_slot(llvm_ctx, RES_OP()->var);

	if (OP1_OP_TYPE() == IS_CONST &&
	    EXPECTED(Z_TYPE_P(RT_CONSTANT(llvm_ctx.op_array, *OP1_OP())) == IS_STRING)) {
		zval *value = RT_CONSTANT(llvm_ctx.op_array, *OP1_OP());
		Value *str_len = LLVM_GET_LONG(Z_STRLEN_P(value));
		zend_jit_save_zval_lval(llvm_ctx, result, RES_SSA_VAR(), RES_INFO(), str_len);
		zend_jit_save_zval_type_info(llvm_ctx, result, RES_SSA_VAR(), RES_INFO(), llvm_ctx.builder.getInt32(IS_LONG));
	} else {
		//JIT: value = GET_OP1_ZVAL_PTR_UNDEF(BP_VAR_R);
		Value *val_ptr = NULL;

		if (OP1_OP_TYPE() == IS_CONST) {
			val_ptr = zend_jit_load_const(llvm_ctx, RT_CONSTANT(llvm_ctx.op_array, *OP1_OP()));
		} else {
			val_ptr = zend_jit_load_slot(llvm_ctx, OP1_OP()->var);
		}
		Value *orig_val_ptr = val_ptr;
		BasicBlock *bb_quit = NULL;
		BasicBlock *bb_finish = NULL;
		Value *val_type = NULL;
		PHI_DCL(str_len, 2);

		if (OP1_INFO() & MAY_BE_STRING) {
			BasicBlock *bb_follow = NULL;
			if (OP1_INFO() & ((MAY_BE_ANY - MAY_BE_STRING) | MAY_BE_UNDEF)) {
				BasicBlock *bb_string = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

				val_type = zend_jit_load_type(llvm_ctx, val_ptr, OP1_SSA_VAR(), OP1_INFO());
				zend_jit_expected_br(llvm_ctx,
						llvm_ctx.builder.CreateICmpEQ(
							val_type,
							llvm_ctx.builder.getInt8(IS_STRING)),
						bb_string,
						bb_follow);
				llvm_ctx.builder.SetInsertPoint(bb_string);
			}
			PHI_ADD(str_len, zend_jit_load_str_len(llvm_ctx,
						zend_jit_load_str(llvm_ctx, val_ptr, OP1_SSA_VAR(), OP1_INFO())));
			if (bb_follow) {
				bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				llvm_ctx.builder.CreateBr(bb_finish);
				llvm_ctx.builder.SetInsertPoint(bb_follow);
			}
		}

		if (OP1_OP_TYPE() == IS_CV && (OP1_INFO() & MAY_BE_UNDEF)) {
			BasicBlock *bb_follow = NULL;
			if (OP1_INFO() & MAY_BE_DEF) {
				BasicBlock *bb_undef = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				//JIT: if (UNEXPECTED(Z_TYPE_P(value) == IS_UNDEF)) {
				if (!val_type) {
					val_type = zend_jit_load_type(llvm_ctx, val_ptr, OP1_SSA_VAR(), OP1_INFO());
				}
				zend_jit_expected_br(llvm_ctx,
						llvm_ctx.builder.CreateICmpEQ(
							val_type,
							llvm_ctx.builder.getInt8(IS_UNDEF)),
						bb_undef,
						bb_follow);
				llvm_ctx.builder.SetInsertPoint(bb_undef);
			}
			//JIT: value = GET_OP1_UNDEF_CV(value, BP_VAR_R);
			JIT_CHECK(zend_jit_undef_cv(llvm_ctx, OP1_OP()->var, opline));
			zend_jit_save_zval_type_info(llvm_ctx, val_ptr, OP1_SSA_VAR(),OP1_INFO(), llvm_ctx.builder.getInt32(IS_NULL));

			if (bb_finish) {
				llvm_ctx.builder.CreateBr(bb_follow);
				llvm_ctx.builder.SetInsertPoint(bb_follow);
			}
		}

		if ((OP1_OP_TYPE() & IS_VAR|IS_CV) && (OP1_INFO() & MAY_BE_REF)) {
			//JIT: if ((Z_TYPE_P(value) == IS_REFERENCE)
			//JIT: value = Z_REFVAL_P(value);
			val_ptr = zend_jit_deref(llvm_ctx, val_ptr, OP1_SSA_VAR(), OP1_INFO());
			//JIT: if (EXPECTED(Z_TYPE_P(value) == IS_STRING)) {
			BasicBlock *bb_string = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

			val_type = zend_jit_load_type(llvm_ctx, val_ptr, OP1_SSA_VAR(), OP1_INFO());
			zend_jit_expected_br(llvm_ctx,
					llvm_ctx.builder.CreateICmpEQ(
						val_type,
						llvm_ctx.builder.getInt8(IS_STRING)),
					bb_string,
					bb_follow);
			llvm_ctx.builder.SetInsertPoint(bb_string);
			//JIT: ZVAL_LONG(EX_VAR(opline->result.var), Z_STRLEN_P(value));
			PHI_ADD(str_len, zend_jit_load_str_len(llvm_ctx,
						zend_jit_load_str(llvm_ctx, val_ptr, OP1_SSA_VAR(), OP1_INFO())));
			if (!bb_finish) {
				bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(bb_finish);
			llvm_ctx.builder.SetInsertPoint(bb_follow);
		}

		if (OP1_INFO() & (MAY_BE_ANY - MAY_BE_STRING)) {
			if (!llvm_ctx.valid_opline) {
				JIT_CHECK(zend_jit_store_opline(llvm_ctx, opline, false));
			}
			Value *ret = zend_jit_slow_strlen(llvm_ctx, val_ptr, result);
			BasicBlock *bb_error = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_quit = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			zend_jit_unexpected_br(
				llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(ret, llvm_ctx.builder.getInt32(0)),
				bb_error,
				bb_quit);
			llvm_ctx.builder.SetInsertPoint(bb_error);
			//JIT: HANDLE_EXCEPTION();
			llvm_ctx.builder.CreateBr(zend_jit_find_exception_bb(llvm_ctx, opline));
		}

		if (bb_finish) {
			Value *len;
			llvm_ctx.builder.SetInsertPoint(bb_finish);

			PHI_SET(str_len, len, LLVM_GET_LONG_TY(llvm_ctx.context));

			zend_jit_save_zval_lval(llvm_ctx, result, RES_SSA_VAR(), RES_INFO(), len);
			zend_jit_save_zval_type_info(llvm_ctx, result, RES_SSA_VAR(), RES_INFO(), llvm_ctx.builder.getInt32(IS_LONG));

			if (bb_quit) {
				llvm_ctx.builder.CreateBr(bb_quit);
			}
		}

		if (bb_quit) {
			llvm_ctx.builder.SetInsertPoint(bb_quit);
		}

		if (!zend_jit_free_operand(llvm_ctx, OP1_OP_TYPE(), orig_val_ptr, NULL, OP1_SSA_VAR(), OP1_INFO(), opline->lineno)) {
			return 0;
		}

	}

	llvm_ctx.valid_opline = 0;
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_concat */
static int zend_jit_concat(zend_llvm_ctx     &llvm_ctx,
                           zend_jit_context  *ctx,
                           zend_op_array     *op_array,
                           zend_op           *opline)
{
	Value *op1;
	Value *op2;
	Value *op1_addr = zend_jit_load_operand(llvm_ctx,
			        OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO(), 0, opline);
	Value *op2_addr = zend_jit_load_operand(llvm_ctx,
			        OP2_OP_TYPE(), OP2_OP(), OP2_SSA_VAR(), OP2_INFO(), 0, opline);
	Value *result_addr = zend_jit_load_slot(llvm_ctx, RES_OP()->var);
	zend_bool may_threw = 0;

	op1 = zend_jit_deref(llvm_ctx, op1_addr, OP1_SSA_VAR(), OP1_INFO());
	op2 = zend_jit_deref(llvm_ctx, op2_addr, OP2_SSA_VAR(), OP2_INFO());

	if (!zend_jit_concat_function(llvm_ctx,
				zend_jit_load_operand_scope(llvm_ctx, OP1_SSA_VAR(), OP1_OP_TYPE(), ctx, op_array),
				op1_addr,
				op1,
				OP1_SSA_VAR(),
				OP1_INFO(),
				OP1_OP_TYPE(),
				OP1_OP(),
				zend_jit_load_operand_scope(llvm_ctx, OP2_SSA_VAR(), OP2_OP_TYPE(), ctx, op_array),
				op2_addr,
				op2,
				OP2_SSA_VAR(),
				OP2_INFO(),
				OP2_OP_TYPE(),
				OP2_OP(),
				result_addr,
				RES_SSA_VAR(),
				RES_INFO(),
				IS_TMP_VAR,
				RES_OP(),
				opline,
				&may_threw)) {
					return 0;
				}

	llvm_ctx.valid_opline = 0;

	return 1;
}
/* }}} */

/* {{{ static int zend_jit_assign_dim */
static int zend_jit_assign_dim(zend_llvm_ctx     &llvm_ctx,
                               zend_jit_context  *ctx,
                               zend_op_array     *op_array,
                               zend_op           *opline)
{
	Value *result;
	Value *dim = NULL;
	BasicBlock *bb_finish = NULL;
	BasicBlock *bb_follow = NULL;
	Value *should_free = NULL;
	Value *op1_addr = NULL;
	Value *op1_type = NULL;
	Value *op2_addr = NULL;
	Value *var_ptr;
	zend_bool may_threw = 0;

	//JIT: object_ptr = GET_OP1_ZVAL_PTR_PTR(BP_VAR_W);
	op1_addr = zend_jit_load_operand_addr(llvm_ctx,
 		OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO(), 0, opline, 0, BP_VAR_W,
 		&should_free);

	if (OP1_MAY_BE(MAY_BE_IN_REG)) {
		op1_addr = zend_jit_reload_from_reg(llvm_ctx, OP1_SSA_VAR(), OP1_INFO());
	}

	if (opline->op1_type == IS_VAR) {
		BasicBlock *bb_error = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		//JIT: if (UNEXPECTED(object_ptr == NULL)) {
		zend_jit_unexpected_br(llvm_ctx,
				llvm_ctx.builder.CreateIsNull(op1_addr),
				bb_error,
				bb_follow);
		llvm_ctx.builder.SetInsertPoint(bb_error);
		//JIT: zend_error_noreturn(E_ERROR, "Cannot use string offset as an array");
		zend_jit_error_noreturn(llvm_ctx, opline, E_ERROR,
				"Cannot use string offset as an array");
		llvm_ctx.builder.SetInsertPoint(bb_follow);
	}

	//JIT: ZVAL_DEREF(object_ptr);
	var_ptr = zend_jit_deref(llvm_ctx, op1_addr, OP1_SSA_VAR(), OP1_INFO());

	if (OP2_OP_TYPE() != IS_UNUSED) {
		op2_addr = zend_jit_load_operand(llvm_ctx,
				OP2_OP_TYPE(), OP2_OP(), OP2_SSA_VAR(), OP2_INFO(), 0, opline);
	}

	if (RETURN_VALUE_USED(opline)) {
		result = zend_jit_load_tmp_zval(llvm_ctx, RES_OP()->var);
	}

	if (OP1_MAY_BE(MAY_BE_OBJECT)) {
		if (OP1_MAY_BE(MAY_BE_ANY - MAY_BE_OBJECT)) {
			BasicBlock *bb_object = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			op1_type = zend_jit_load_type(llvm_ctx, var_ptr, OP1_SSA_VAR(), OP1_INFO());
			zend_jit_unexpected_br(
					llvm_ctx, 
					llvm_ctx.builder.CreateICmpEQ(
						op1_type,
						llvm_ctx.builder.getInt8(IS_OBJECT)),
					bb_object,
					bb_follow);
			llvm_ctx.builder.SetInsertPoint(bb_object);
		}

		if (op2_addr) {
			dim = op2_addr;
		} else {
			dim = llvm_ctx.builder.CreateIntToPtr(LLVM_GET_LONG(0), llvm_ctx.zval_ptr_type);
		}

		zend_jit_assign_to_object(
				llvm_ctx,
				zend_jit_load_operand_scope(llvm_ctx, OP1_SSA_VAR(), OP1_OP_TYPE(), ctx, op_array),
				var_ptr,
				op1_type,
				OP1_SSA_VAR(),
				OP1_INFO(),
				OP1_OP_TYPE(),
				dim,
				OP2_SSA_VAR(),
				OP2_INFO(),
				OP2_OP(),
				OP2_OP_TYPE(),
				RETURN_VALUE_USED(opline) ?
					zend_jit_load_slot(llvm_ctx, RES_OP()->var) : NULL,
				RETURN_VALUE_USED(opline) ?
					RES_SSA_VAR() : -1,
				RETURN_VALUE_USED(opline) ?
					RES_INFO() : MAY_BE_ANY,
				NULL,
				ZEND_ASSIGN_DIM,
				op_array,
				opline,
				&may_threw);

		if (!bb_finish) {
			bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		}

		llvm_ctx.builder.CreateBr(bb_finish);
	}

	if (OP1_MAY_BE(MAY_BE_ANY - MAY_BE_OBJECT)) {
		Value *ret;
		Value *op_data;
		Value *value;
		Value *offset;
		Value *new_element;
		BasicBlock *bb_string_offset = NULL;
		BasicBlock *bb_new_element = NULL;
		BasicBlock *bb_uninitialized = NULL;
		BasicBlock *bb_error = NULL;

		if (bb_follow) {
			llvm_ctx.builder.SetInsertPoint(bb_follow);
		}

		if (op2_addr) {
			dim = op2_addr;
		}

		op_data = zend_jit_load_operand(llvm_ctx,
				OP1_DATA_OP_TYPE(), OP1_DATA_OP(), OP1_DATA_SSA_VAR(), OP1_DATA_INFO(), 0, opline);
		value = zend_jit_deref(llvm_ctx, op_data, OP1_DATA_SSA_VAR(), OP1_DATA_INFO());

		ret = zend_jit_fetch_dimension_address(
				llvm_ctx,
				NULL,
				NULL,
				var_ptr,
				NULL,
				OP1_SSA_VAR(),
				OP1_INFO() & ~MAY_BE_OBJECT,
				OP1_OP(),
				OP1_OP_TYPE(),
				dim,
				OP2_SSA_VAR(),
				OP2_INFO(),
				OP2_OP(),
				OP2_OP_TYPE(),
				BP_VAR_W,
				&offset,
				&bb_string_offset,
				&new_element,
				&bb_new_element,
				NULL,
				NULL,
				&bb_uninitialized,
				&bb_error,
				1,
				opline, 
				&may_threw);

		if (ret) {
			zend_jit_assign_to_variable(
					llvm_ctx,
					op_array,
					ret,
					-1 & ~(MAY_BE_ERROR|MAY_BE_IN_REG),
					-1,
					-1 & ~(MAY_BE_ERROR|MAY_BE_IN_REG),
					-1,
					IS_VAR,
					NULL,
					value,
					OP1_DATA_INFO(),
					OP1_DATA_SSA_VAR(),
					OP1_DATA_OP_TYPE(),
					OP1_DATA_OP(),
					opline);

			if (RETURN_VALUE_USED(opline)) {
				zend_jit_copy_value(
						llvm_ctx,
						result,
						0,
						RES_SSA_VAR(),
						RES_INFO(),
						ret,
						NULL,
						OP1_DATA_OP_TYPE(),
						OP1_DATA_OP(),
						OP1_DATA_SSA_VAR(),
						OP1_DATA_INFO());
			}

			if (OP1_DATA_OP_TYPE() == IS_CV) {
				zend_jit_update_reg_value(
					llvm_ctx,
					(opline+1)->op1.var,
					value,
					OP1_DATA_SSA_VAR(),
					OP1_DATA_INFO(),
					OP1_DATA_DEF_SSA_VAR(),
					OP1_DATA_DEF_INFO());
			} else if (OP1_DATA_OP_TYPE() == IS_VAR) {
				if (!zend_jit_free_operand(llvm_ctx,
							OP1_DATA_OP_TYPE(), op_data, NULL, OP1_DATA_SSA_VAR(), OP1_DATA_INFO(), opline->lineno, 1)) {
					return 0;
				}
			}

			if (!bb_finish) {
				bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}

			llvm_ctx.builder.CreateBr(bb_finish);
		}

		if (bb_error) {
			llvm_ctx.builder.SetInsertPoint(bb_error);

			if (RETURN_VALUE_USED(opline)) {
				zend_jit_save_zval_type_info(llvm_ctx, result, RES_SSA_VAR(), RES_INFO(), llvm_ctx.builder.getInt32(IS_NULL));
			}

			if (!zend_jit_free_operand(llvm_ctx,
						OP1_DATA_OP_TYPE(), op_data, NULL, OP1_DATA_SSA_VAR(), OP1_DATA_INFO(), opline->lineno)) {
				return 0;
			}

			if (!bb_finish) {
				bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}

			llvm_ctx.builder.CreateBr(bb_finish);
		}

		if (bb_new_element) {
			llvm_ctx.builder.SetInsertPoint(bb_new_element);
			zend_jit_assign_to_variable(
					llvm_ctx,
					op_array,
					new_element,
					MAY_BE_NULL | MAY_BE_RC1,
					-1,
					-1 & ~MAY_BE_IN_REG,
					-1,
					IS_VAR,
					NULL,
					value,
					OP1_DATA_INFO(),
					OP1_DATA_SSA_VAR(),
					OP1_DATA_OP_TYPE(),
					OP1_DATA_OP(),
					opline);

			if (RETURN_VALUE_USED(opline)) {
				zend_jit_copy_value(
						llvm_ctx,
						result,
						0,
						RES_SSA_VAR(),
						RES_INFO(),
						new_element,
						NULL,
						OP1_DATA_OP_TYPE(),
						OP1_DATA_OP(),
						OP1_DATA_SSA_VAR(),
						OP1_DATA_INFO());
			}

			if (OP1_DATA_OP_TYPE() == IS_CV) {
				zend_jit_update_reg_value(
					llvm_ctx,
					(opline+1)->op1.var,
					value,
					OP1_DATA_SSA_VAR(),
					OP1_DATA_INFO(),
					OP1_DATA_DEF_SSA_VAR(),
					OP1_DATA_DEF_INFO());
			} else if (OP1_DATA_OP_TYPE() == IS_VAR) {
				if (!zend_jit_free_operand(llvm_ctx,
							OP1_DATA_OP_TYPE(), op_data, NULL, OP1_DATA_SSA_VAR(), OP1_DATA_INFO(), opline->lineno)) {
					return 0;
				}
			}

			if (!bb_finish) {
				bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(bb_finish);
		}

		if (bb_string_offset) {
			llvm_ctx.builder.SetInsertPoint(bb_string_offset);
			zend_jit_assign_to_string_offset(
					llvm_ctx,
					var_ptr,
					offset,
					value,
					(RETURN_VALUE_USED(opline))?
					zend_jit_load_slot(llvm_ctx, RES_OP()->var):
					llvm_ctx.builder.CreateIntToPtr(
						LLVM_GET_LONG(0),
						llvm_ctx.zval_ptr_type),
					opline);

			if (!zend_jit_free_operand(llvm_ctx,
						OP1_DATA_OP_TYPE(), value, NULL, OP1_DATA_SSA_VAR(), OP1_DATA_INFO(), opline->lineno)) {
				return 0;
			}

			if (!bb_finish) {
				bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(bb_finish);
		}

		if (bb_uninitialized) {
			llvm_ctx.builder.SetInsertPoint(bb_uninitialized);
			if (RETURN_VALUE_USED(opline)) {
				zend_jit_save_zval_type_info(llvm_ctx, result, RES_SSA_VAR(), RES_INFO(), llvm_ctx.builder.getInt32(IS_NULL));
			}

			if (!zend_jit_free_operand(llvm_ctx,
						OP1_DATA_OP_TYPE(), value, NULL, OP1_DATA_SSA_VAR(), OP1_DATA_INFO(), opline->lineno)) {
				return 0;
			}

			if (!bb_finish) {
				bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(bb_finish);
		}
	}

	if (bb_finish) {
		llvm_ctx.builder.SetInsertPoint(bb_finish);
	}

	if (op2_addr && !zend_jit_free_operand(llvm_ctx, OP2_OP_TYPE(), op2_addr, NULL, OP2_SSA_VAR(), OP2_INFO(), opline->lineno)) {
	   	return 0;
	}

	if  (opline->op1_type == IS_CV) {
		zend_jit_update_reg_value(
			llvm_ctx,
			opline->op1.var,
			op1_addr,
			OP1_SSA_VAR(),
			OP1_INFO(),
			OP1_DEF_SSA_VAR(),
			OP1_DEF_INFO());
	} else if (opline->op1_type == IS_VAR && should_free) {
		zend_jit_free_var_ptr(llvm_ctx, should_free, OP1_SSA_VAR(), OP1_INFO(), opline);
	}

	JIT_CHECK(zend_jit_check_exception(llvm_ctx, opline));

	llvm_ctx.valid_opline = 0;
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_assign_obj */
static int zend_jit_assign_obj(zend_llvm_ctx     &llvm_ctx,
                               zend_jit_context  *ctx,
                               zend_op_array     *op_array,
                               int                bb,
                               zend_op           *opline)
{
	Value *property;
	BasicBlock *bb_follow = NULL;
	Value *op1_addr = NULL;
	zend_bool may_threw = 0;

	if (OP1_OP_TYPE() == IS_VAR) {
		return zend_jit_handler(llvm_ctx, opline);
	}

	op1_addr = zend_jit_load_operand(llvm_ctx,
			OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO(), 0, opline, 1, BP_VAR_W);

	if (opline->op1_type == IS_UNUSED && zend_jit_needs_check_for_this(llvm_ctx, bb)) {
		BasicBlock *bb_error = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		zend_jit_unexpected_br(llvm_ctx,
			llvm_ctx.builder.CreateIsNull(zend_jit_load_obj(llvm_ctx, op1_addr, -1, MAY_BE_OBJECT)),
			bb_error,
			bb_follow);
		llvm_ctx.builder.SetInsertPoint(bb_error);

		//JIT: zend_throw_error(NULL, "Using $this when not in object context");
		zend_jit_throw_error(llvm_ctx, opline, NULL,
					"Using $this when not in object context");

		//JIT: FREE_UNFETCHED_OP2();
		if (opline->op2_type & (IS_VAR|IS_TMP_VAR)) {
			Value *op2_addr = zend_jit_load_operand(llvm_ctx,
				OP2_OP_TYPE(), OP2_OP(), OP2_SSA_VAR(), OP2_INFO(), 0, opline);
			zend_jit_zval_ptr_dtor_ex(llvm_ctx, op2_addr, NULL, OP2_SSA_VAR(), OP2_INFO(), opline->lineno, 0);
		}

		llvm_ctx.builder.CreateBr(zend_jit_find_exception_bb(llvm_ctx, opline));

		llvm_ctx.builder.SetInsertPoint(bb_follow);
	}

	property = zend_jit_load_operand(llvm_ctx,
			OP2_OP_TYPE(), OP2_OP(), OP2_SSA_VAR(), OP2_INFO(), 0, opline);

	zend_jit_assign_to_object(
			llvm_ctx,
			zend_jit_load_operand_scope(llvm_ctx, OP1_SSA_VAR(), OP1_OP_TYPE(), ctx, op_array),
			op1_addr,
			NULL,
			OP1_SSA_VAR(),
			OP1_OP_TYPE() == IS_UNUSED? MAY_BE_OBJECT : OP1_INFO(),
			OP1_OP_TYPE(),
			property,
			OP2_SSA_VAR(),
			OP2_INFO(),
			OP2_OP(),
			OP2_OP_TYPE(),
			RETURN_VALUE_USED(opline)? zend_jit_load_slot(llvm_ctx, RES_OP()->var) : NULL,
			RETURN_VALUE_USED(opline)? RES_SSA_VAR() : -1,
			RETURN_VALUE_USED(opline)? RES_INFO() : 0,
			(OP2_OP_TYPE() == IS_CONST)?
			zend_jit_cache_slot_addr(
				llvm_ctx,
				Z_CACHE_SLOT_P(RT_CONSTANT(llvm_ctx.op_array, *OP2_OP())),
				PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))) : 
			llvm_ctx.builder.CreateIntToPtr(
				LLVM_GET_LONG(0),
				PointerType::getUnqual(PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context)))),
			ZEND_ASSIGN_OBJ,
			op_array,
			opline,
			&may_threw);

	if (!zend_jit_free_operand(llvm_ctx, OP2_OP_TYPE(), property, NULL, OP2_SSA_VAR(), OP2_INFO(), opline->lineno)) {
	   	return 0;
	}

	if (!zend_jit_free_operand(llvm_ctx, OP1_OP_TYPE(), op1_addr, NULL, OP1_SSA_VAR(), OP1_INFO(), opline->lineno)) {
	   	return 0;
	}

	llvm_ctx.valid_opline = 0;
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_assign_op */
static int zend_jit_assign_op(zend_llvm_ctx     &llvm_ctx,
                              zend_jit_context  *ctx,
                              zend_op_array     *op_array,
                              zend_op           *opline,
                              zend_uchar         assign_type)
{
	Value *should_free;
	Value *op1;
	Value *op1_addr;
	Value *op2_addr = NULL;
	Value *var_ptr;
	Value *var_type = NULL;
	Value *val_addr = NULL;
	Value *val_ptr = NULL;
	int same_cvs = 0;
	uint32_t var_info, var_def_info;
	int var_ssa_var, var_def_ssa_var;
	zend_uchar var_op_type = 0;
	znode_op *var_op = NULL;
	uint32_t val_info;
	uint32_t val_ssa_var;
	zend_uchar val_op_type;
	znode_op *val_op;
	zend_bool do_assign = 0;
	zend_bool may_threw = 0;
	BasicBlock *bb_finish = NULL;
	BasicBlock *bb_follow = NULL;

	if (opline->extended_value == ZEND_ASSIGN_OBJ ||
	    (opline->extended_value == ZEND_ASSIGN_DIM && OP1_MAY_BE(MAY_BE_OBJECT))) {
		return zend_jit_handler(llvm_ctx, opline);
	}

 	op1_addr = zend_jit_load_operand_addr(llvm_ctx,
 		OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO(), 0, opline, 0, BP_VAR_RW,
 		&should_free);

	if (OP1_OP_TYPE() != IS_UNUSED) {
		op1 = zend_jit_deref(llvm_ctx, op1_addr, OP1_SSA_VAR(), OP1_INFO());
	} else {
		op1 = op1_addr;
	}

	if (OP2_OP_TYPE() != IS_UNUSED) {
		op2_addr = zend_jit_load_operand(llvm_ctx, OP2_OP_TYPE(), OP2_OP(), OP2_SSA_VAR(), OP2_INFO(), 0, opline);
	}

	switch (opline->extended_value) {
		case ZEND_ASSIGN_OBJ:
			break;
		case ZEND_ASSIGN_DIM:
			{
				if (OP1_MAY_BE(MAY_BE_OBJECT)) {
					ASSERT_NOT_REACHED();
				}

				if (OP1_MAY_BE(MAY_BE_ANY - MAY_BE_OBJECT)) {
					Value *ret;
					Value *dim = NULL;
					Value *new_element = NULL;
					Value *offset = NULL;
					PHI_DCL(var_ptr, 2);
					BasicBlock *bb_string_offset = NULL;
					BasicBlock *bb_new_element = NULL;
					BasicBlock *bb_error = NULL;
					BasicBlock *bb_merge_value = NULL;

					val_addr = val_ptr = zend_jit_load_operand(llvm_ctx, 
							OP1_DATA_OP_TYPE(), OP1_DATA_OP(), OP1_DATA_SSA_VAR(), OP1_DATA_INFO(), 0, opline);

					val_info = OP1_DATA_INFO();
					val_ssa_var = OP1_DATA_SSA_VAR();
					val_op_type = OP1_DATA_OP_TYPE();
					val_op = OP1_DATA_OP();

					if (op2_addr) {
						dim = zend_jit_deref(llvm_ctx, op2_addr, OP2_SSA_VAR(), OP2_INFO());
					}

					ret = zend_jit_fetch_dimension_address(
							llvm_ctx,
							NULL,
							NULL,
							op1,
							NULL,
							OP1_SSA_VAR(),
							OP1_INFO() & ~MAY_BE_OBJECT,
							OP1_OP(),
							OP1_OP_TYPE(),
							dim,
							OP2_SSA_VAR(),
							OP2_INFO(),
							OP2_OP(),
							OP2_OP_TYPE(),
							BP_VAR_RW,
							&offset,
							&bb_string_offset,
							&new_element,
							&bb_new_element,
							NULL,
							NULL,
							NULL,
							&bb_error,
							1,
							opline, 
							&may_threw);

					if (ret) {
						do_assign = 1;
						var_ptr = ret;
						var_info = array_element_type(OP1_INFO(), 1, OP2_OP_TYPE() == IS_UNUSED) & ~MAY_BE_ERROR;
						var_ssa_var = -1;
						var_def_info = array_element_type(OP1_DEF_INFO(), 1, OP2_OP_TYPE() == IS_UNUSED) & ~MAY_BE_ERROR;
						var_def_ssa_var = -1;
						if (bb_new_element) {
							PHI_ADD(var_ptr, ret);
							bb_merge_value = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
							llvm_ctx.builder.CreateBr(bb_merge_value);
						} else {
							if (!bb_follow) {
								bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
							}
							llvm_ctx.builder.CreateBr(bb_follow);
						}
					}

					if (bb_new_element) {
						do_assign = 1;
						llvm_ctx.builder.SetInsertPoint(bb_new_element);
						if (ret) {
							PHI_ADD(var_ptr, new_element);
							var_info |= MAY_BE_NULL;
							llvm_ctx.builder.CreateBr(bb_merge_value);
						} else {
							var_ptr = new_element;
							var_info = MAY_BE_NULL;
							if (!bb_follow) {
								bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
							}
							llvm_ctx.builder.CreateBr(bb_follow);
						}
					}

					if (bb_string_offset) {
						llvm_ctx.builder.SetInsertPoint(bb_string_offset);
						zend_jit_error_noreturn(
								llvm_ctx,
								opline,
								E_ERROR,
								"%s",
								LLVM_GET_CONST_STRING(
									"Cannot use assign-op operators with overloaded objects nor string offsets"));
					}

					if (bb_error) {
						llvm_ctx.builder.SetInsertPoint(bb_error);
						if (RETURN_VALUE_USED(opline)) {
							zend_jit_save_zval_type_info(
									llvm_ctx,
									zend_jit_load_tmp_zval(llvm_ctx, RES_OP()->var),
									RES_SSA_VAR(), RES_INFO(),
									llvm_ctx.builder.getInt32(IS_NULL));
						}
						if (!bb_finish) {
							bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
						}
						llvm_ctx.builder.CreateBr(bb_finish);
					}

					if (bb_merge_value) {
						llvm_ctx.builder.SetInsertPoint(bb_merge_value);
						PHI_SET(var_ptr, var_ptr, llvm_ctx.zval_ptr_type);
						if (!bb_follow) {
							bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
						}
						llvm_ctx.builder.CreateBr(bb_follow);
					}
				}
			}
			break;
		default:
			var_ptr = op1;
			var_info = OP1_INFO();
			var_ssa_var = OP1_SSA_VAR();
			var_def_info = OP1_DEF_INFO();
			var_def_ssa_var = OP1_DEF_SSA_VAR();
			var_op_type = OP1_OP_TYPE();
			var_op  = OP1_OP();

			val_ptr = zend_jit_deref(llvm_ctx, op2_addr, OP2_SSA_VAR(), OP2_INFO());
			val_info = OP2_INFO();
			val_ssa_var = OP2_SSA_VAR();
			val_op_type = OP2_OP_TYPE();
			val_op = OP2_OP();

			same_cvs = SAME_CVs(opline);
			do_assign = 1;
			break;
	}

	if (do_assign) {
		if (bb_follow) {
			llvm_ctx.builder.SetInsertPoint(bb_follow);
			bb_follow = NULL;
		}

		zend_jit_separate_zval_noref(llvm_ctx, var_ptr, var_type, var_op_type, var_op, var_ssa_var, var_info, opline);

		if (var_info & MAY_BE_OBJECT) {
			zend_class_entry *scope = zend_jit_load_operand_scope(llvm_ctx, var_ssa_var, var_op_type, ctx, op_array);

			if (IS_CUSTOM_HANDLERS(scope)) {
				Value *obj;
				Value *object_handlers;
				Value *get_handler;
				Value *set_handler;
				Value *retval;
				BasicBlock *bb_cont;
				bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				if (var_info & (MAY_BE_ANY - MAY_BE_OBJECT)) {
					BasicBlock *bb_object = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					if (!var_type) {
						var_type = zend_jit_load_type(llvm_ctx, var_ptr, var_ssa_var, var_info);
					}
					zend_jit_expected_br(
							llvm_ctx,
							llvm_ctx.builder.CreateICmpEQ(
								var_type,
								llvm_ctx.builder.getInt8(IS_OBJECT)),
							bb_object,
							bb_follow);
					llvm_ctx.builder.SetInsertPoint(bb_object);
				}

				obj = zend_jit_load_obj(llvm_ctx, var_ptr, var_ssa_var, var_info);
				object_handlers = zend_jit_load_obj_handlers(llvm_ctx, obj);
				get_handler = zend_jit_load_obj_handler(llvm_ctx, object_handlers, get);

				bb_cont = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

				zend_jit_expected_br(llvm_ctx,
						llvm_ctx.builder.CreateIsNull(get_handler),
						bb_follow,
						bb_cont);
				llvm_ctx.builder.SetInsertPoint(bb_cont);

				set_handler = zend_jit_load_obj_handler(llvm_ctx, object_handlers, set);

				bb_cont = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				zend_jit_expected_br(llvm_ctx,
						llvm_ctx.builder.CreateIsNull(set_handler),
						bb_follow,
						bb_cont);
				llvm_ctx.builder.SetInsertPoint(bb_cont);

				if (OP2_MAY_BE(MAY_BE_IN_REG)) {
					Value *val_ptr = zend_jit_reload_from_reg(llvm_ctx, OP2_SSA_VAR(), OP2_INFO());
					retval = zend_jit_obj_proxy_op(llvm_ctx, var_ptr, val_ptr, assign_type, opline);
				} else {
					retval = zend_jit_obj_proxy_op(llvm_ctx, var_ptr, val_ptr, assign_type, opline);
				}

				if (RETURN_VALUE_USED(opline)) {
					Value *result = zend_jit_load_tmp_zval(llvm_ctx, RES_OP()->var);
					zend_jit_copy_value(
							llvm_ctx,
							result,
							0,
							RES_SSA_VAR(),
							RES_INFO(),
							retval,
							NULL,
							IS_VAR,
							NULL,
							-1,
							RES_INFO() & MAY_BE_ANY);
				}

				if (!bb_finish) {
					bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				}
				llvm_ctx.builder.CreateBr(bb_finish);
			}
		}

		if (bb_follow) {
			llvm_ctx.builder.SetInsertPoint(bb_follow);
		}

		switch (assign_type) {
			case ZEND_ADD:
			case ZEND_SUB:
			case ZEND_MUL:
			case ZEND_DIV:
				zend_jit_math(
						llvm_ctx,
						NULL,
						var_ptr,
						var_op_type,
						var_op,
						var_ssa_var,
						var_info,
						val_addr,
						val_ptr,
						val_op_type,
						val_op,
						val_ssa_var,
						val_info,
						var_ptr,
						var_op,
						var_def_ssa_var,
						var_def_info,
						same_cvs,
						opline->lineno,
						assign_type,
						opline,
						0);
				break;
			case ZEND_CONCAT:
				zend_jit_concat_function(
						llvm_ctx,
						NULL,
						NULL,
						var_ptr,
						var_ssa_var,
						var_info,
						var_op_type,
						var_op,
						NULL,
						val_addr,
						val_ptr,
						val_ssa_var,
						val_info,
						val_op_type,
						val_op,
						var_ptr,
						var_def_ssa_var,
						var_def_info,
						var_op_type,
						var_op,
						opline,
						&may_threw);
				break;
			default:
				ASSERT_NOT_REACHED();
				break;
		}

		if (RETURN_VALUE_USED(opline)) {
			Value *result = zend_jit_load_tmp_zval(llvm_ctx, RES_OP()->var);
			zend_jit_copy_value(
					llvm_ctx,
					result,
					0,
					RES_SSA_VAR(),
					RES_INFO(),
					var_ptr,
					NULL,
					IS_VAR,
					NULL,
					-1,
					RES_INFO() & MAY_BE_ANY);
		}
	}

	if (bb_finish) {
		llvm_ctx.builder.CreateBr(bb_finish);
		llvm_ctx.builder.SetInsertPoint(bb_finish);
	}

	if (opline->op1_type == IS_VAR && should_free) {
		zend_jit_free_var_ptr(llvm_ctx, should_free, OP1_SSA_VAR(), OP1_INFO(), opline);
	}

	if (!zend_jit_free_operand(llvm_ctx, OP2_OP_TYPE(), op2_addr, NULL, OP2_SSA_VAR(), OP2_INFO(), opline->lineno)) {
		return 0;
	}

	llvm_ctx.valid_opline = 0;
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_isset_isempty_dim_obj */
static int zend_jit_isset_isempty_dim_obj(zend_llvm_ctx     &llvm_ctx,
                                          zend_jit_context  *ctx,
                                          zend_op_array     *op_array,
                                          int                bb,
                                          zend_op           *opline)
{
	Value *container;
	Value *offset;
	Value *result;
	Value *should_free = NULL;
	Value *op1_addr = NULL;
	Value *op2_addr = NULL;
	Value *op1_type = NULL;
	Value *op2_type = NULL;
	BasicBlock *bb_follow = NULL;
	BasicBlock *bb_finish = NULL;
	PHI_DCL(result, 16);

	op1_addr = zend_jit_load_operand_addr(llvm_ctx,
 		OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO(), 0, opline, 1, BP_VAR_IS,
 		&should_free);

	if (opline->op1_type == IS_UNUSED && zend_jit_needs_check_for_this(llvm_ctx, bb)) {
		BasicBlock *bb_error = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		zend_jit_unexpected_br(llvm_ctx,
			llvm_ctx.builder.CreateIsNull(zend_jit_load_obj(llvm_ctx, op1_addr, -1, MAY_BE_OBJECT)),
			bb_error,
			bb_follow);
		llvm_ctx.builder.SetInsertPoint(bb_error);

		//JIT: zend_throw_error(NULL, "Using $this when not in object context");
		zend_jit_throw_error(llvm_ctx, opline, NULL,
					"Using $this when not in object context");

		//JIT: FREE_UNFETCHED_OP2();
		if (opline->op2_type & (IS_VAR|IS_TMP_VAR)) {
			Value *op2_addr = zend_jit_load_operand(llvm_ctx,
				OP2_OP_TYPE(), OP2_OP(), OP2_SSA_VAR(), OP2_INFO(), 0, opline);
			zend_jit_zval_ptr_dtor_ex(llvm_ctx, op2_addr, NULL, OP2_SSA_VAR(), OP2_INFO(), opline->lineno, 0);
		}

		llvm_ctx.builder.CreateBr(zend_jit_find_exception_bb(llvm_ctx, opline));

		llvm_ctx.builder.SetInsertPoint(bb_follow);
	}

	container = zend_jit_deref(llvm_ctx, op1_addr, OP1_SSA_VAR(), OP1_INFO());

	op2_addr = zend_jit_load_operand(llvm_ctx, OP2_OP_TYPE(), OP2_OP(), OP2_SSA_VAR(), OP2_INFO(), 0, opline);
	offset = zend_jit_deref(llvm_ctx, op2_addr, OP2_SSA_VAR(), OP2_INFO());

	if (OP1_OP_TYPE() != IS_UNUSED) {
		Value *str = NULL;
		BasicBlock *bb_cont = NULL;

		if (OP1_MAY_BE(MAY_BE_ARRAY)) {
			Value *ht;
			Value *val = NULL;
			Value *hval = NULL;
			BasicBlock *bb_cal_ret = NULL;
			PHI_DCL(val, 2);

			if (OP1_MAY_BE(MAY_BE_ANY - MAY_BE_ARRAY)) {
				BasicBlock *bb_array = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				if (!op1_type) {
					op1_type = zend_jit_load_type(llvm_ctx, container, OP1_SSA_VAR(), OP1_INFO());
				}
				zend_jit_expected_br(llvm_ctx,
						llvm_ctx.builder.CreateICmpEQ(
							op1_type,
							llvm_ctx.builder.getInt8(IS_ARRAY)),
						bb_array,
						bb_follow);
				llvm_ctx.builder.SetInsertPoint(bb_array);
			}

			ht = zend_jit_load_array_ht(
					llvm_ctx,
					zend_jit_load_array(llvm_ctx, container, OP1_SSA_VAR(), OP1_INFO()));

			if (OP2_OP_TYPE() == IS_CONST) {
				switch (Z_TYPE_P(RT_CONSTANT(llvm_ctx.op_array, *OP2_OP()))) {
					case IS_STRING:
						str = llvm_ctx.builder.CreateIntToPtr(
								LLVM_GET_LONG((zend_uintptr_t)Z_STR_P(RT_CONSTANT(llvm_ctx.op_array, *OP2_OP()))),
								PointerType::getUnqual(llvm_ctx.zend_string_type));
						break;
					case IS_LONG:
						hval = LLVM_GET_LONG(Z_LVAL_P(RT_CONSTANT(llvm_ctx.op_array, *OP2_OP())));
						break;
					case IS_DOUBLE:
						hval = LLVM_GET_LONG(zend_dval_to_lval(Z_DVAL_P(RT_CONSTANT(llvm_ctx.op_array, *OP2_OP()))));
						break;
					case IS_NULL:
						str = llvm_ctx.builder.CreateIntToPtr(
								LLVM_GET_LONG((zend_uintptr_t)STR_EMPTY_ALLOC()),
								PointerType::getUnqual(llvm_ctx.zend_string_type));
						break;
					case IS_FALSE:
						hval = LLVM_GET_LONG(0);
						break;
					case IS_TRUE:
						hval = LLVM_GET_LONG(1);
						break;
					default:
						zend_jit_error(
								llvm_ctx,
								opline,
								E_WARNING,
								"%s",
								LLVM_GET_CONST_STRING("Illegal offset type in isset or empty"));
						break;
				}

				if (str) {
					val = zend_jit_hash_find(llvm_ctx, ht, str);
				} else if (hval) {
					val = zend_jit_hash_index_find(llvm_ctx, ht, hval);
				} else {
					if (opline->extended_value & ZEND_ISSET) {
						PHI_ADD(result, llvm_ctx.builder.getInt32(IS_FALSE));
					} else {
						PHI_ADD(result, llvm_ctx.builder.getInt32(IS_TRUE));
					}
					if (!bb_finish) {
						bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					}
					llvm_ctx.builder.CreateBr(bb_finish);
				}
			} else {
				PHI_DCL(str_offset, 2);
				PHI_DCL(num_index, 6);
				BasicBlock *bb_str_offset = NULL;
				BasicBlock *bb_num_index = NULL;

				if (OP2_MAY_BE(MAY_BE_STRING)) {
					Value *zstr;
					Value *zhval;
					BasicBlock *bb_handle_string = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					BasicBlock *bb_handle_numeric = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

					if (OP2_MAY_BE(MAY_BE_ANY-MAY_BE_STRING)) {
						BasicBlock *bb_string = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
						bb_cont = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
						if (!op2_type) {
							op2_type = zend_jit_load_type(llvm_ctx, offset, OP2_SSA_VAR(), OP2_INFO());
						}
						zend_jit_expected_br(llvm_ctx,
								llvm_ctx.builder.CreateICmpEQ(
									op2_type,
									llvm_ctx.builder.getInt8(IS_STRING)),
								bb_string,
								bb_cont);
						llvm_ctx.builder.SetInsertPoint(bb_string);
					}
					zstr = zend_jit_load_str(llvm_ctx, offset, OP2_SSA_VAR(), OP2_INFO());
					zhval = llvm_ctx.builder.CreateBitCast(
							zend_jit_get_stack_slot(llvm_ctx, 0),
							PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context)));
					zend_jit_handle_numeric(llvm_ctx,
							zstr,
							zhval,
							bb_handle_numeric,
							bb_handle_string,
							opline);
					llvm_ctx.builder.SetInsertPoint(bb_handle_numeric);
					PHI_ADD(num_index, llvm_ctx.builder.CreateAlignedLoad(zhval, 4));
					if (!bb_num_index) {
						bb_num_index = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					}
					llvm_ctx.builder.CreateBr(bb_num_index);
					llvm_ctx.builder.SetInsertPoint(bb_handle_string);
					PHI_ADD(str_offset, zstr);
					if (!bb_str_offset) {
						bb_str_offset = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					}
					llvm_ctx.builder.CreateBr(bb_str_offset);
				}

				if (OP2_MAY_BE(MAY_BE_LONG)) {
					if (bb_cont) {
						llvm_ctx.builder.SetInsertPoint(bb_cont);
						bb_cont = NULL;
					}

					if (OP2_MAY_BE(MAY_BE_ANY-(MAY_BE_LONG|MAY_BE_STRING))) {
						BasicBlock *bb_long = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
						bb_cont = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
						if (!op2_type) {
							op2_type = zend_jit_load_type(llvm_ctx, offset, OP2_SSA_VAR(), OP2_INFO());
						}
						zend_jit_expected_br(llvm_ctx,
								llvm_ctx.builder.CreateICmpEQ(
									op2_type,
									llvm_ctx.builder.getInt8(IS_LONG)),
								bb_long,
								bb_cont);
						llvm_ctx.builder.SetInsertPoint(bb_long);
					}
					PHI_ADD(num_index, zend_jit_load_lval(llvm_ctx, offset, OP2_SSA_VAR(), OP2_INFO()));
					if (!bb_num_index) {
						bb_num_index = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					}
					llvm_ctx.builder.CreateBr(bb_num_index);
				}

				if (OP2_MAY_BE(MAY_BE_DOUBLE)) {
					if (bb_cont) {
						llvm_ctx.builder.SetInsertPoint(bb_cont);
						bb_cont = NULL;
					}

					if (OP2_MAY_BE(MAY_BE_ANY-(MAY_BE_DOUBLE|MAY_BE_LONG|MAY_BE_STRING))) {
						BasicBlock *bb_double = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
						bb_cont = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
						if (!op2_type) {
							op2_type = zend_jit_load_type(llvm_ctx, offset, OP2_SSA_VAR(), OP2_INFO());
						}
						zend_jit_expected_br(llvm_ctx,
								llvm_ctx.builder.CreateICmpEQ(
									op2_type,
									llvm_ctx.builder.getInt8(IS_DOUBLE)),
								bb_double,
								bb_cont);
						llvm_ctx.builder.SetInsertPoint(bb_double);
					}
					PHI_ADD(num_index, 
							zend_jit_dval_to_lval(llvm_ctx, 
								zend_jit_load_dval(llvm_ctx, offset, OP2_SSA_VAR(), OP2_INFO())));
					if (!bb_num_index) {
						bb_num_index = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					}
					llvm_ctx.builder.CreateBr(bb_num_index);
				}

				if (OP2_MAY_BE(MAY_BE_NULL)) {
					if (bb_cont) {
						llvm_ctx.builder.SetInsertPoint(bb_cont);
						bb_cont = NULL;
					}

					if (OP2_MAY_BE(MAY_BE_ANY-(MAY_BE_NULL|MAY_BE_DOUBLE|MAY_BE_LONG|MAY_BE_STRING))) {
						BasicBlock *bb_null = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
						bb_cont = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
						if (!op2_type) {
							op2_type = zend_jit_load_type(llvm_ctx, offset, OP2_SSA_VAR(), OP2_INFO());
						}
						zend_jit_expected_br(llvm_ctx,
								llvm_ctx.builder.CreateICmpEQ(
									op2_type,
									llvm_ctx.builder.getInt8(IS_NULL)),
								bb_null,
								bb_cont);
						llvm_ctx.builder.SetInsertPoint(bb_null);
					}
					PHI_ADD(str_offset, llvm_ctx._CG_empty_string);
					if (!bb_str_offset) {
						bb_str_offset = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					}
					llvm_ctx.builder.CreateBr(bb_str_offset);
				}

				if (OP2_MAY_BE(MAY_BE_FALSE)) {
					if (bb_cont) {
						llvm_ctx.builder.SetInsertPoint(bb_cont);
						bb_cont = NULL;
					}

					if (OP2_MAY_BE(MAY_BE_ANY-(MAY_BE_FALSE|MAY_BE_NULL|MAY_BE_DOUBLE|MAY_BE_LONG|MAY_BE_STRING))) {
						BasicBlock *bb_false = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
						bb_cont = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
						if (!op2_type) {
							op2_type = zend_jit_load_type(llvm_ctx, offset, OP2_SSA_VAR(), OP2_INFO());
						}
						zend_jit_expected_br(llvm_ctx,
								llvm_ctx.builder.CreateICmpEQ(
									op2_type,
									llvm_ctx.builder.getInt8(IS_FALSE)),
								bb_false,
								bb_cont);
						llvm_ctx.builder.SetInsertPoint(bb_false);
					}
					PHI_ADD(num_index, LLVM_GET_LONG(0));
					if (!bb_num_index) {
						bb_num_index = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					}
					llvm_ctx.builder.CreateBr(bb_num_index);
				}

				if (OP2_MAY_BE(MAY_BE_TRUE)) {
					if (bb_cont) {
						llvm_ctx.builder.SetInsertPoint(bb_cont);
						bb_cont = NULL;
					}

					if (OP2_MAY_BE(MAY_BE_ANY-
								(MAY_BE_TRUE|MAY_BE_FALSE|MAY_BE_NULL|MAY_BE_DOUBLE|MAY_BE_LONG|MAY_BE_STRING))) {
						BasicBlock *bb_true = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
						bb_cont = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
						if (!op2_type) {
							op2_type = zend_jit_load_type(llvm_ctx, offset, OP2_SSA_VAR(), OP2_INFO());
						}
						zend_jit_expected_br(llvm_ctx,
								llvm_ctx.builder.CreateICmpEQ(
									op2_type,
									llvm_ctx.builder.getInt8(IS_TRUE)),
								bb_true,
								bb_cont);
						llvm_ctx.builder.SetInsertPoint(bb_true);
					}
					PHI_ADD(num_index, LLVM_GET_LONG(1));
					if (!bb_num_index) {
						bb_num_index = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					}
					llvm_ctx.builder.CreateBr(bb_num_index);
				}

				if (OP2_MAY_BE(MAY_BE_RESOURCE)) {
					if (bb_cont) {
						llvm_ctx.builder.SetInsertPoint(bb_cont);
						bb_cont = NULL;
					}

					if (OP2_MAY_BE(MAY_BE_ANY-
								(MAY_BE_RESOURCE|MAY_BE_TRUE|MAY_BE_FALSE|
								 MAY_BE_NULL|MAY_BE_DOUBLE|MAY_BE_LONG|MAY_BE_STRING))) {
						BasicBlock *bb_res = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
						bb_cont = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
						if (!op2_type) {
							op2_type = zend_jit_load_type(llvm_ctx, offset, OP2_SSA_VAR(), OP2_INFO());
						}
						zend_jit_expected_br(llvm_ctx,
								llvm_ctx.builder.CreateICmpEQ(
									op2_type,
									llvm_ctx.builder.getInt8(IS_RESOURCE)),
								bb_res,
								bb_cont);
						llvm_ctx.builder.SetInsertPoint(bb_res);
					}
					PHI_ADD(num_index, zend_jit_load_res_handle(
								llvm_ctx,
								zend_jit_load_res(llvm_ctx, op2_addr, OP2_SSA_VAR(), OP2_INFO())));
					if (!bb_num_index) {
						bb_num_index = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					}
					llvm_ctx.builder.CreateBr(bb_num_index);
				}
				/* IS_REFERENCE is handled */
				if (bb_cont ||
						OP2_MAY_BE(MAY_BE_ANY-
							(MAY_BE_RESOURCE|MAY_BE_TRUE|MAY_BE_FALSE|
							 MAY_BE_NULL|MAY_BE_DOUBLE|MAY_BE_LONG|MAY_BE_STRING))) {
					if (bb_cont) {
						llvm_ctx.builder.SetInsertPoint(bb_cont);
					}
					zend_jit_error(
							llvm_ctx,
							opline,
							E_WARNING,
							"%s",
							LLVM_GET_CONST_STRING("Illegal offset type in isset or empty"));
					if ((opline->extended_value & ZEND_ISSET) == 0) {
						PHI_ADD(result, llvm_ctx.builder.getInt32(IS_TRUE));
					} else {
						PHI_ADD(result, llvm_ctx.builder.getInt32(IS_FALSE));
					}
					if (!bb_finish) {
						bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					}
					llvm_ctx.builder.CreateBr(bb_finish);
				}

				if (bb_str_offset) {
					llvm_ctx.builder.SetInsertPoint(bb_str_offset);
					PHI_SET(str_offset, str, PointerType::getUnqual(llvm_ctx.zend_string_type));
					val = zend_jit_hash_find(llvm_ctx, ht, str);
					PHI_ADD(val, val);
					if (!bb_cal_ret) {
						bb_cal_ret = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					}
					llvm_ctx.builder.CreateBr(bb_cal_ret);
				}

				if (bb_num_index) {
					llvm_ctx.builder.SetInsertPoint(bb_num_index);
					PHI_SET(num_index, hval, LLVM_GET_LONG_TY(llvm_ctx.context));
					val = zend_jit_hash_index_find(llvm_ctx, ht, hval);
					PHI_ADD(val, val);
					if (!bb_cal_ret) {
						bb_cal_ret = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					}
					llvm_ctx.builder.CreateBr(bb_cal_ret);
				}
			}

			if (val) {
				Value *val_ind;
				Value *val_type;
				BasicBlock *bb_null = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				BasicBlock *bb_ind = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				BasicBlock *bb_next = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				BasicBlock *bb_next2 = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				PHI_DCL(val_type, 2);
				PHI_DCL(real_val, 2);

				if (bb_cal_ret) {
					llvm_ctx.builder.SetInsertPoint(bb_cal_ret);
					PHI_SET(val, val, llvm_ctx.zval_ptr_type);
				}

				zend_jit_unexpected_br(llvm_ctx,
						llvm_ctx.builder.CreateIsNull(val),
						bb_null,
						bb_next);
				llvm_ctx.builder.SetInsertPoint(bb_null);
				if (opline->extended_value & ZEND_ISSET) {
					PHI_ADD(result, llvm_ctx.builder.getInt32(IS_FALSE));
				} else {
					PHI_ADD(result, llvm_ctx.builder.getInt32(IS_TRUE));
				}

				if (!bb_finish) {
					bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				}
				llvm_ctx.builder.CreateBr(bb_finish);

				llvm_ctx.builder.SetInsertPoint(bb_next);
				PHI_ADD(real_val, val);
				val_type = zend_jit_load_type(llvm_ctx, val, -1, MAY_BE_ANY);
				PHI_ADD(val_type, val_type);
				bb_next = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				zend_jit_unexpected_br(llvm_ctx,
						llvm_ctx.builder.CreateICmpEQ(
							val_type,
							llvm_ctx.builder.getInt8(IS_INDIRECT)),
						bb_ind,
						bb_next);
				llvm_ctx.builder.SetInsertPoint(bb_ind);
				val_ind = zend_jit_load_ind(llvm_ctx, val);
				bb_null = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				zend_jit_unexpected_br(llvm_ctx,
						llvm_ctx.builder.CreateIsNull(val_ind),
						bb_null,
						bb_next2);
				llvm_ctx.builder.SetInsertPoint(bb_null);
				if (opline->extended_value & ZEND_ISSET) {
					PHI_ADD(result, llvm_ctx.builder.getInt32(IS_FALSE));
				} else {
					PHI_ADD(result, llvm_ctx.builder.getInt32(IS_TRUE));
				}

				if (!bb_finish) {
					bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				}
				llvm_ctx.builder.CreateBr(bb_finish);
				llvm_ctx.builder.SetInsertPoint(bb_next2);

				PHI_ADD(real_val, val_ind);
				PHI_ADD(val_type, zend_jit_load_type(llvm_ctx, val_ind, -1, MAY_BE_ANY));
				llvm_ctx.builder.CreateBr(bb_next);

				llvm_ctx.builder.SetInsertPoint(bb_next);
				PHI_SET(real_val, val, llvm_ctx.zval_ptr_type);
				if (opline->extended_value & ZEND_ISSET) {
					Value *val_ref;
					BasicBlock *bb_null = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					BasicBlock *bb_ref = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					BasicBlock *bb_ref_null = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					BasicBlock *bb_next = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

					PHI_SET(val_type, val_type, Type::getInt8Ty(llvm_ctx.context));

					/* JIT: result = value != NULL && Z_TYPE_P(value) > IS_NULL &&
					   (!Z_ISREF_P(value) || Z_TYPE_P(Z_REFVAL_P(value)) != IS_NULL); */
					zend_jit_expected_br(llvm_ctx,
							llvm_ctx.builder.CreateICmpSGT(
								val_type,
								llvm_ctx.builder.getInt8(IS_NULL)),
							bb_next,
							bb_null);
					llvm_ctx.builder.SetInsertPoint(bb_null);
					PHI_ADD(result, llvm_ctx.builder.getInt32(IS_FALSE));
					llvm_ctx.builder.CreateBr(bb_finish);

					llvm_ctx.builder.SetInsertPoint(bb_next);
					bb_next = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					zend_jit_expected_br(llvm_ctx,
							llvm_ctx.builder.CreateICmpNE(
								val_type,
								llvm_ctx.builder.getInt8(IS_REFERENCE)),
							bb_next,
							bb_ref);
					llvm_ctx.builder.SetInsertPoint(bb_ref);
					val_ref = zend_jit_deref(llvm_ctx, val, -1, MAY_BE_REF);

					zend_jit_unexpected_br(llvm_ctx,
							llvm_ctx.builder.CreateICmpEQ(
								zend_jit_load_type(llvm_ctx, val_ref, -1, MAY_BE_ANY),
								llvm_ctx.builder.getInt8(IS_NULL)),
							bb_ref_null,
							bb_next);
					llvm_ctx.builder.SetInsertPoint(bb_ref_null);
					PHI_ADD(result, llvm_ctx.builder.getInt32(IS_FALSE));
					llvm_ctx.builder.CreateBr(bb_finish);
					llvm_ctx.builder.SetInsertPoint(bb_next);
					PHI_ADD(result, llvm_ctx.builder.getInt32(IS_TRUE));
				} else /* if (opline->extended_value & ZEND_ISEMPTY) */ {
					BasicBlock *bb_true = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					BasicBlock *bb_false = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					BasicBlock *bb_set = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					Value *ret = zend_jit_is_true(llvm_ctx, val);

					PHI_DCL(bool_val, 2);
					zend_jit_unexpected_br(llvm_ctx,
							llvm_ctx.builder.CreateICmpEQ(
								ret, 
								llvm_ctx.builder.getInt32(0)),
							bb_false,
							bb_true);
					llvm_ctx.builder.SetInsertPoint(bb_false);
					PHI_ADD(bool_val, llvm_ctx.builder.getInt32(IS_TRUE));
					llvm_ctx.builder.CreateBr(bb_set);
					llvm_ctx.builder.SetInsertPoint(bb_true);
					PHI_ADD(bool_val, llvm_ctx.builder.getInt32(IS_FALSE));
					llvm_ctx.builder.CreateBr(bb_set);
					llvm_ctx.builder.SetInsertPoint(bb_set);
					PHI_SET(bool_val, ret, Type::getInt32Ty(llvm_ctx.context));
					PHI_ADD(result, ret);
				}
				llvm_ctx.builder.CreateBr(bb_finish);
			} else {
				/* ???
				if (opline->extended_value & ZEND_ISSET) {
					PHI_ADD(result, llvm_ctx.builder.getInt32(IS_FALSE));
				} else {
					PHI_ADD(result, llvm_ctx.builder.getInt32(IS_TRUE));
				}
				if (!bb_finish) {
					bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				}
				llvm_ctx.builder.CreateBr(bb_finish);
				*/
			}
		}

		if (OP1_MAY_BE(MAY_BE_OBJECT)) {
			Value *obj;
			Value *object_handlers;
			Value *has_dim_handler;
			zend_class_entry *scope;
			BasicBlock *bb_has_dim = NULL;
			BasicBlock *bb_cont = NULL;

			if (bb_follow) {
				llvm_ctx.builder.SetInsertPoint(bb_follow);
				bb_follow = NULL;
			}

			if (OP1_MAY_BE(MAY_BE_ANY-(MAY_BE_OBJECT|MAY_BE_ARRAY))) {
				BasicBlock *bb_object = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				if (!op1_type) {
					op1_type = zend_jit_load_type(llvm_ctx, container, OP1_SSA_VAR(), OP1_INFO());
				}
				zend_jit_expected_br(llvm_ctx,
						llvm_ctx.builder.CreateICmpEQ(
							op1_type,
							llvm_ctx.builder.getInt8(IS_OBJECT)),
						bb_object,
						bb_follow);
				llvm_ctx.builder.SetInsertPoint(bb_object);
			}

			scope = zend_jit_load_operand_scope(llvm_ctx, OP1_SSA_VAR(), OP1_OP_TYPE(), ctx, op_array);

			if (IS_CUSTOM_HANDLERS(scope)) {
				obj = zend_jit_load_obj(llvm_ctx, container, OP1_SSA_VAR(), OP1_INFO());
				object_handlers = zend_jit_load_obj_handlers(llvm_ctx, obj);
				has_dim_handler = zend_jit_load_obj_handler(llvm_ctx, object_handlers, has_dimension);
				bb_has_dim = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				bb_cont = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

				zend_jit_expected_br(llvm_ctx,
						llvm_ctx.builder.CreateIsNotNull(has_dim_handler),
						bb_has_dim,
						bb_cont);
			} else {
				/* static int zend_std_has_dimension(zval *object, zval *offset, int check_empty TSRMLS_DC) */
				has_dim_handler = zend_jit_get_helper(
						llvm_ctx,
						(void*)std_object_handlers.has_dimension,
						ZEND_JIT_SYM("zend_std_has_dimension"),
						0,
						PointerType::getInt32Ty(llvm_ctx.context),
						llvm_ctx.zval_ptr_type,
						llvm_ctx.zval_ptr_type,
						PointerType::getInt32Ty(llvm_ctx.context),
						NULL,
						NULL);
			}

			if (bb_has_dim) {
				Value *ret;
				BasicBlock *bb_true = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				BasicBlock *bb_false = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				BasicBlock *bb_set = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				llvm_ctx.builder.SetInsertPoint(bb_has_dim);
				PHI_DCL(bool_val, 2);

				if (OP2_MAY_BE(MAY_BE_IN_REG)) {
					Value *offset = zend_jit_reload_from_reg(llvm_ctx, OP2_SSA_VAR(), OP2_INFO());
					ret = zend_jit_has_dimension(llvm_ctx, has_dim_handler,
							container, offset, (opline->extended_value & ZEND_ISSET) == 0, opline);
				} else {
					ret = zend_jit_has_dimension(llvm_ctx, has_dim_handler,
							container, offset, (opline->extended_value & ZEND_ISSET) == 0, opline);
				}

				zend_jit_unexpected_br(llvm_ctx,
						llvm_ctx.builder.CreateICmpEQ(
							ret, 
							llvm_ctx.builder.getInt32(0)),
						bb_false,
						bb_true);
				llvm_ctx.builder.SetInsertPoint(bb_false);
				if (opline->extended_value & ZEND_ISSET) {
					PHI_ADD(bool_val, llvm_ctx.builder.getInt32(IS_FALSE));
				} else {
					PHI_ADD(bool_val, llvm_ctx.builder.getInt32(IS_TRUE));
				}
				llvm_ctx.builder.CreateBr(bb_set);
				llvm_ctx.builder.SetInsertPoint(bb_true);
				if (opline->extended_value & ZEND_ISSET) {
					PHI_ADD(bool_val, llvm_ctx.builder.getInt32(IS_TRUE));
				} else {
					PHI_ADD(bool_val, llvm_ctx.builder.getInt32(IS_FALSE));
				}
				llvm_ctx.builder.CreateBr(bb_set);
				llvm_ctx.builder.SetInsertPoint(bb_set);
				PHI_SET(bool_val, ret, Type::getInt32Ty(llvm_ctx.context));
				PHI_ADD(result, ret);

				if (!bb_finish) {
					bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				}
				llvm_ctx.builder.CreateBr(bb_finish);
			}

			if (bb_cont) {
				llvm_ctx.builder.SetInsertPoint(bb_cont);
				zend_jit_error(
						llvm_ctx,
						opline,
						E_NOTICE,
						"%s",
						LLVM_GET_CONST_STRING("Trying to check element of non-array"));
				if ((opline->extended_value & ZEND_ISSET)) {
					PHI_ADD(result, llvm_ctx.builder.getInt32(IS_FALSE));
				} else {
					PHI_ADD(result, llvm_ctx.builder.getInt32(IS_TRUE));
				}
				if (!bb_finish) {
					bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				}
				llvm_ctx.builder.CreateBr(bb_finish);
			}
		}

		if (OP1_MAY_BE(MAY_BE_STRING)) {
			Value *str;
			Value *str_len;
			Value *index = NULL;

			if (bb_follow) {
				llvm_ctx.builder.SetInsertPoint(bb_follow);
				bb_follow = NULL;
			}

			if (OP1_MAY_BE(MAY_BE_ANY-(MAY_BE_STRING|MAY_BE_OBJECT|MAY_BE_ARRAY))) {
				BasicBlock *bb_string = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				if (!op1_type) {
					op1_type = zend_jit_load_type(llvm_ctx, container, OP1_SSA_VAR(), OP1_INFO());
				}
				zend_jit_expected_br(llvm_ctx,
						llvm_ctx.builder.CreateICmpEQ(
							op1_type,
							llvm_ctx.builder.getInt8(IS_STRING)),
						bb_string,
						bb_follow);
				llvm_ctx.builder.SetInsertPoint(bb_string);
			}

			if (OP2_OP_TYPE() == IS_CONST) {
				if (Z_TYPE_P(RT_CONSTANT(llvm_ctx.op_array, *OP2_OP())) < IS_STRING /* simple scalar types */
						|| (Z_TYPE_P(RT_CONSTANT(llvm_ctx.op_array, *OP2_OP())) == IS_STRING /* or numeric string */
							&& IS_LONG == is_numeric_string(
								Z_STRVAL_P(RT_CONSTANT(llvm_ctx.op_array, *OP2_OP())), Z_STRLEN_P(RT_CONSTANT(llvm_ctx.op_array, *OP2_OP())), NULL, NULL, 0))) {
					index = LLVM_GET_LONG(_zval_get_long_func(RT_CONSTANT(llvm_ctx.op_array, *OP2_OP())));
				} else {
					if (opline->extended_value & ZEND_ISSET) {
						PHI_ADD(result, llvm_ctx.builder.getInt32(IS_FALSE));
					} else {
						PHI_ADD(result, llvm_ctx.builder.getInt32(IS_TRUE));
					}
					if (!bb_finish) {
						bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					}
					llvm_ctx.builder.CreateBr(bb_finish);
				}
			} else {
				if (OP2_MAY_BE(MAY_BE_ANY - MAY_BE_LONG)) {
					Value *zhval;
					PHI_DCL(hval, 3);
					BasicBlock *bb_long = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					BasicBlock *bb_not_long = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					BasicBlock *bb_handle_long = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					BasicBlock *bb_convert_long = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					BasicBlock *bb_cont = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					BasicBlock *bb_cal_index = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					BasicBlock *bb_no_exists = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

					op2_type = zend_jit_load_type(llvm_ctx, offset, OP2_SSA_VAR(), OP2_INFO());
					zend_jit_unexpected_br(llvm_ctx,
							llvm_ctx.builder.CreateICmpNE(
								op2_type,
								llvm_ctx.builder.getInt8(IS_LONG)),
							bb_not_long,
							bb_long);
					llvm_ctx.builder.SetInsertPoint(bb_not_long);

					zend_jit_expected_br(llvm_ctx,
							llvm_ctx.builder.CreateICmpSLT(
								op2_type,
								llvm_ctx.builder.getInt8(IS_STRING)),
							bb_convert_long,
							bb_cont);
					llvm_ctx.builder.SetInsertPoint(bb_cont);
					bb_cont = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					zend_jit_expected_br(llvm_ctx,
							llvm_ctx.builder.CreateICmpEQ(
								op2_type,
								llvm_ctx.builder.getInt8(IS_STRING)),
							bb_cont,
							bb_no_exists);
					llvm_ctx.builder.SetInsertPoint(bb_cont);

					zhval = llvm_ctx.builder.CreateBitCast(
							zend_jit_get_stack_slot(llvm_ctx, 0),
							PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context)));

					zend_jit_handle_numeric(llvm_ctx,
							zend_jit_load_str(llvm_ctx, offset, OP2_SSA_VAR(), OP2_INFO()),
							zhval,
							bb_no_exists,
							bb_handle_long,
							opline);
					llvm_ctx.builder.SetInsertPoint(bb_no_exists);
					if (opline->extended_value & ZEND_ISSET) {
						PHI_ADD(result, llvm_ctx.builder.getInt32(IS_FALSE));
					} else {
						PHI_ADD(result, llvm_ctx.builder.getInt32(IS_TRUE));
					}
					if (!bb_finish) {
						bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					}
					llvm_ctx.builder.CreateBr(bb_finish);

					llvm_ctx.builder.SetInsertPoint(bb_handle_long);
					PHI_ADD(hval, llvm_ctx.builder.CreateAlignedLoad(zhval, 4));
					llvm_ctx.builder.CreateBr(bb_cal_index);

					llvm_ctx.builder.SetInsertPoint(bb_convert_long);
					PHI_ADD(hval, zend_jit_zval_get_lval_func(llvm_ctx, offset));
					llvm_ctx.builder.CreateBr(bb_cal_index);

					llvm_ctx.builder.SetInsertPoint(bb_long);
					PHI_ADD(hval, zend_jit_load_lval(llvm_ctx, offset, OP2_SSA_VAR(), OP2_INFO()));
					llvm_ctx.builder.CreateBr(bb_cal_index);

					llvm_ctx.builder.SetInsertPoint(bb_cal_index);

					PHI_SET(hval, index, LLVM_GET_LONG_TY(llvm_ctx.context));

				} else {
					index = zend_jit_load_lval(llvm_ctx, offset, OP2_SSA_VAR(), OP2_INFO());
				}
			}

			if (index) {
				BasicBlock *bb_cont = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				BasicBlock *bb_invalid_len = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				zend_jit_expected_br(llvm_ctx,
						llvm_ctx.builder.CreateICmpSGE(
							index,
							LLVM_GET_LONG(0)),
						bb_cont,
						bb_invalid_len);
				llvm_ctx.builder.SetInsertPoint(bb_cont);

				bb_cont = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				str = zend_jit_load_str(llvm_ctx, container, OP1_SSA_VAR(), OP1_INFO());
				str_len = zend_jit_load_str_len(llvm_ctx, str);
				zend_jit_expected_br(llvm_ctx,
						llvm_ctx.builder.CreateICmpSLT(
							index,
							str_len),
						bb_cont,
						bb_invalid_len);
				llvm_ctx.builder.SetInsertPoint(bb_cont);

				if ((opline->extended_value & ZEND_ISSET) == 0) {
					Value *str_val = zend_jit_load_str_val(llvm_ctx, str);
					Value *str_char = llvm_ctx.builder.CreateAlignedLoad(
							llvm_ctx.builder.CreateGEP(str_val, index), 1);
					bb_cont = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					zend_jit_unexpected_br(llvm_ctx,
							llvm_ctx.builder.CreateICmpNE(
								str_char,
								llvm_ctx.builder.getInt8('0')),
							bb_cont,
							bb_invalid_len);
					llvm_ctx.builder.SetInsertPoint(bb_cont);
					PHI_ADD(result, llvm_ctx.builder.getInt32(IS_FALSE));
				} else {
					PHI_ADD(result, llvm_ctx.builder.getInt32(IS_TRUE));
				}
				if (!bb_finish) {
					bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				}
				llvm_ctx.builder.CreateBr(bb_finish);
				llvm_ctx.builder.SetInsertPoint(bb_invalid_len);
				if (opline->extended_value & ZEND_ISSET) {
					PHI_ADD(result, llvm_ctx.builder.getInt32(IS_FALSE));
				} else {
					PHI_ADD(result, llvm_ctx.builder.getInt32(IS_TRUE));
				}
				llvm_ctx.builder.CreateBr(bb_finish);
			}
		}

		if (bb_follow || OP1_MAY_BE(MAY_BE_ANY-(MAY_BE_STRING|MAY_BE_OBJECT|MAY_BE_ARRAY))) {
			if (bb_follow) {
				llvm_ctx.builder.SetInsertPoint(bb_follow);
			}

			if (opline->extended_value & ZEND_ISSET) {
				PHI_ADD(result, llvm_ctx.builder.getInt32(IS_FALSE));
			} else {
				PHI_ADD(result, llvm_ctx.builder.getInt32(IS_TRUE));
			}

			if (!bb_finish) {
				bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(bb_finish);
		}
	} else {
		Value *obj;
		Value *object_handlers;
		Value *has_dim_handler;
		zend_class_entry *scope;
		BasicBlock *bb_has_dim = NULL;
		BasicBlock *bb_cont = NULL;

		scope = zend_jit_load_operand_scope(llvm_ctx, OP1_SSA_VAR(), OP1_OP_TYPE(), ctx, op_array);

		if (IS_CUSTOM_HANDLERS(scope)) {
			obj = zend_jit_load_obj(llvm_ctx, container, OP1_SSA_VAR(), OP1_INFO());
			object_handlers = zend_jit_load_obj_handlers(llvm_ctx, obj);
			has_dim_handler = zend_jit_load_obj_handler(llvm_ctx, object_handlers, has_dimension);
			bb_has_dim = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_cont = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

			zend_jit_expected_br(llvm_ctx,
					llvm_ctx.builder.CreateIsNotNull(has_dim_handler),
					bb_has_dim,
					bb_cont);
		} else {
			/* static int zend_std_has_dimension(zval *object, zval *offset, int check_empty TSRMLS_DC) */
			has_dim_handler = zend_jit_get_helper(
					llvm_ctx,
					(void*)std_object_handlers.has_dimension,
					ZEND_JIT_SYM("zend_std_has_dimension"),
					0,
					PointerType::getInt32Ty(llvm_ctx.context),
					llvm_ctx.zval_ptr_type,
					llvm_ctx.zval_ptr_type,
					PointerType::getInt32Ty(llvm_ctx.context),
					NULL,
					NULL);
		}

		if (bb_has_dim) {
			Value *ret;
			BasicBlock *bb_true = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			BasicBlock *bb_false = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			BasicBlock *bb_set = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			llvm_ctx.builder.SetInsertPoint(bb_has_dim);
			PHI_DCL(bool_val, 2);

			ret = zend_jit_has_dimension(llvm_ctx, has_dim_handler,
					container, offset, (opline->extended_value & ZEND_ISSET) == 0, opline);

			zend_jit_unexpected_br(llvm_ctx,
					llvm_ctx.builder.CreateICmpEQ(
						ret, 
						llvm_ctx.builder.getInt32(0)),
					bb_false,
					bb_true);
			llvm_ctx.builder.SetInsertPoint(bb_false);
			if (opline->extended_value & ZEND_ISSET) {
				PHI_ADD(bool_val, llvm_ctx.builder.getInt32(IS_FALSE));
			} else {
				PHI_ADD(bool_val, llvm_ctx.builder.getInt32(IS_TRUE));
			}
			llvm_ctx.builder.CreateBr(bb_set);
			llvm_ctx.builder.SetInsertPoint(bb_true);
			if (opline->extended_value & ZEND_ISSET) {
				PHI_ADD(bool_val, llvm_ctx.builder.getInt32(IS_TRUE));
			} else {
				PHI_ADD(bool_val, llvm_ctx.builder.getInt32(IS_FALSE));
			}
			llvm_ctx.builder.CreateBr(bb_set);
			llvm_ctx.builder.SetInsertPoint(bb_set);
			PHI_SET(bool_val, ret, Type::getInt32Ty(llvm_ctx.context));
			PHI_ADD(result, ret);

			if (!bb_finish) {
				bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(bb_finish);
		}

		if (bb_cont) {
			llvm_ctx.builder.SetInsertPoint(bb_cont);
			zend_jit_error(
					llvm_ctx,
					opline,
					E_NOTICE,
					"%s",
					LLVM_GET_CONST_STRING("Trying to check element of non-array"));
			if ((opline->extended_value & ZEND_ISSET)) {
				PHI_ADD(result, llvm_ctx.builder.getInt32(IS_FALSE));
			} else {
				PHI_ADD(result, llvm_ctx.builder.getInt32(IS_TRUE));
			}
			if (!bb_finish) {
				bb_finish = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(bb_finish);
		}
	}

	if (bb_finish) {
		Value *res;
		llvm_ctx.builder.SetInsertPoint(bb_finish);
		PHI_SET(result, result, Type::getInt32Ty(llvm_ctx.context));

		res = zend_jit_load_tmp_zval(llvm_ctx, RES_OP()->var);
		zend_jit_save_zval_type_info(llvm_ctx, res, RES_SSA_VAR(), RES_INFO(), result);
	}

	if (!zend_jit_free_operand(llvm_ctx, OP2_OP_TYPE(), op2_addr, NULL, OP2_SSA_VAR(), OP2_INFO(), opline->lineno))
	{
		return 0;
	}

	//JIT: FREE_OP1_VAR_PTR();
	if (opline->op1_type == IS_VAR && should_free) {
		zend_jit_free_var_ptr(llvm_ctx, should_free, OP1_SSA_VAR(), OP1_INFO(), opline);
	}

	JIT_CHECK(zend_jit_check_exception(llvm_ctx, opline));
	llvm_ctx.valid_opline = 0;
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_bind_global */
static int zend_jit_bind_global(zend_llvm_ctx     &llvm_ctx,
                                zend_op_array     *op_array,
                                zend_op           *opline)
{
	Value *idx;
	zval *varname;
	Value *variable;
	Value *value;
	Value *val_type;
	Value *val_counted;
	Value *arData;
	Value *symbol_table;
	Value *cache_slot_addr;
	Value *cached_bucket;
	Value *bucket_val;
	Value *bucket_h;
	Value *bucket_key;
	Value *bucket_val_type;
	BasicBlock *bb_valid_idx;
	BasicBlock *bb_follow;
	BasicBlock *bb_cached;
	BasicBlock *bb_add_new;
	BasicBlock *bb_next;
	BasicBlock *bb_check_indirect;
	BasicBlock *bb_indirect;
	BasicBlock *bb_undef;
	BasicBlock *bb_not_same;
	PHI_DCL(value, 2);
	PHI_DCL(retval, 4);

	varname = RT_CONSTANT(llvm_ctx.op_array, *OP2_OP());
	//JIT: idx = (uint32_t)(uintptr_t)CACHED_PTR(Z_CACHE_SLOT_P(varname)) - 1;
	cache_slot_addr = zend_jit_cache_slot_addr(
			llvm_ctx,
			Z_CACHE_SLOT_P(varname),
			Type::getInt32Ty(llvm_ctx.context));

	idx = llvm_ctx.builder.CreateSub(
			llvm_ctx.builder.CreateAlignedLoad(cache_slot_addr, 4),
			llvm_ctx.builder.getInt32(1));

	symbol_table = zend_jit_load_array_ht(llvm_ctx, llvm_ctx._EG_symbol_table);

	bb_valid_idx = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

	//JIT: if (EXPECTED(idx < EG(symbol_table).ht.nNumUsed))
	zend_jit_expected_br(llvm_ctx,
			llvm_ctx.builder.CreateICmpULT(
				idx,
				llvm_ctx.builder.CreateAlignedLoad(
					zend_jit_GEP(
						llvm_ctx,
						symbol_table,
						offsetof(HashTable, nNumUsed),
						PointerType::getUnqual(Type::getInt32Ty(llvm_ctx.context))), 4)),
			bb_valid_idx,
			bb_follow);

	llvm_ctx.builder.SetInsertPoint(bb_valid_idx);

	arData = llvm_ctx.builder.CreateAlignedLoad(
			zend_jit_GEP(
				llvm_ctx,
				symbol_table,
				offsetof(HashTable, arData),
				PointerType::getUnqual(PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context)))), 4);

	cached_bucket = llvm_ctx.builder.CreateIntToPtr(
			llvm_ctx.builder.CreateAdd(
				llvm_ctx.builder.CreatePtrToInt(
					arData,
					LLVM_GET_LONG_TY(llvm_ctx.context)),
				llvm_ctx.builder.CreateZExt(
					llvm_ctx.builder.CreateMul(
						idx,
						llvm_ctx.builder.getInt32(sizeof(Bucket))),
					LLVM_GET_LONG_TY(llvm_ctx.context))),
			PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context)));

	bucket_val = zend_jit_GEP(llvm_ctx,
				cached_bucket,
				offsetof(Bucket, val),
				llvm_ctx.zval_ptr_type);

	bucket_val_type = zend_jit_load_type(llvm_ctx, bucket_val, -1, MAY_BE_ANY);

	bb_next = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	//JIT: if (EXPECTED(Z_TYPE(p->val) != IS_UNDEF) &&
    //        (EXPECTED(p->key == Z_STR_P(varname)) ||
    //         (EXPECTED(p->h == Z_STR_P(varname)->h) &&
    //         EXPECTED(p->key != NULL) &&
    //         EXPECTED(p->key->len == Z_STRLEN_P(varname)) &&
    //         EXPECTED(memcmp(p->key->val, Z_STRVAL_P(varname), Z_STRLEN_P(varname)) == 0))))
	zend_jit_expected_br(llvm_ctx,
			llvm_ctx.builder.CreateICmpNE(
				bucket_val_type,
				llvm_ctx.builder.getInt8(IS_UNDEF)),
			bb_next,
			bb_follow);
	llvm_ctx.builder.SetInsertPoint(bb_next);
	bb_cached = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	bb_next = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	bucket_key = llvm_ctx.builder.CreateAlignedLoad(
			zend_jit_GEP(llvm_ctx,
				cached_bucket,
				offsetof(Bucket, key),
				PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.zend_string_type))), 4);
	zend_jit_expected_br(llvm_ctx,
			llvm_ctx.builder.CreateICmpEQ(
				bucket_key,
				llvm_ctx.builder.CreateIntToPtr(
					LLVM_GET_LONG((zend_uintptr_t)Z_STR_P(varname)),
					PointerType::getUnqual(llvm_ctx.zend_string_type))),
			bb_cached,
			bb_next);
	llvm_ctx.builder.SetInsertPoint(bb_next);
	bucket_h = llvm_ctx.builder.CreateAlignedLoad(
			zend_jit_GEP(llvm_ctx,
				cached_bucket,
				offsetof(Bucket, h),
				PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4);
	bb_next = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	zend_jit_expected_br(llvm_ctx,
			llvm_ctx.builder.CreateICmpEQ(
				bucket_h,
				LLVM_GET_LONG(Z_STR_P(varname)->h)),
			bb_next,
			bb_follow);
	llvm_ctx.builder.SetInsertPoint(bb_next);
	bb_next = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	zend_jit_expected_br(llvm_ctx,
			llvm_ctx.builder.CreateIsNotNull(bucket_key),
			bb_next,
			bb_follow);
	llvm_ctx.builder.SetInsertPoint(bb_next);
	bb_next = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	zend_jit_expected_br(llvm_ctx,
			llvm_ctx.builder.CreateICmpEQ(
				llvm_ctx.builder.CreateAlignedLoad(
					zend_jit_GEP(llvm_ctx,
						bucket_key,
						offsetof(zend_string, len),
						PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4),
				LLVM_GET_LONG(Z_STRLEN_P(varname))),
			bb_next,
			bb_follow);
	llvm_ctx.builder.SetInsertPoint(bb_next);
	zend_jit_expected_br(llvm_ctx,
			llvm_ctx.builder.CreateICmpEQ(
				zend_jit_memcmp(llvm_ctx,
					zend_jit_load_str_val(llvm_ctx, bucket_key),
					llvm_ctx.builder.CreateIntToPtr(
						LLVM_GET_LONG((zend_uintptr_t)Z_STRVAL_P(varname)),
						PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context))),
					LLVM_GET_LONG(Z_STRLEN_P(varname))),
				llvm_ctx.builder.getInt32(0)),
			bb_cached,
			bb_follow);
	llvm_ctx.builder.SetInsertPoint(bb_cached);
			
	//JIT: value = &EG(symbol_table).ht.arData[idx].val;
	PHI_ADD(value, bucket_val);
	bb_check_indirect = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	llvm_ctx.builder.CreateBr(bb_check_indirect);
	llvm_ctx.builder.SetInsertPoint(bb_follow);

	value = zend_jit_hash_find(llvm_ctx, symbol_table, 
			llvm_ctx.builder.CreateIntToPtr(
				LLVM_GET_LONG((zend_uintptr_t)Z_STR_P(varname)),
				PointerType::getUnqual(llvm_ctx.zend_string_type)));

	bb_next = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	bb_add_new = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	zend_jit_unexpected_br(llvm_ctx,
			llvm_ctx.builder.CreateIsNotNull(value),
			bb_next,
			bb_add_new);
	llvm_ctx.builder.SetInsertPoint(bb_next);
	arData = llvm_ctx.builder.CreateAlignedLoad(
			zend_jit_GEP(
				llvm_ctx,
				symbol_table,
				offsetof(HashTable, arData),
				PointerType::getUnqual(PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context)))), 4);
	idx = llvm_ctx.builder.CreateExactSDiv(
			llvm_ctx.builder.CreateSub(
				llvm_ctx.builder.CreatePtrToInt(value, Type::getInt32Ty(llvm_ctx.context)),
				llvm_ctx.builder.CreatePtrToInt(arData, Type::getInt32Ty(llvm_ctx.context))),
			llvm_ctx.builder.getInt32(sizeof(Bucket)));
	llvm_ctx.builder.CreateAlignedStore(
			llvm_ctx.builder.CreateAdd(idx, llvm_ctx.builder.getInt32(1)),
				llvm_ctx.builder.CreateBitCast(
					cache_slot_addr,
					PointerType::getUnqual(Type::getInt32Ty(llvm_ctx.context))), 4);
	PHI_ADD(value, value);
	llvm_ctx.builder.CreateBr(bb_check_indirect);
	llvm_ctx.builder.SetInsertPoint(bb_check_indirect);
	PHI_SET(value, value, llvm_ctx.zval_ptr_type);
	PHI_ADD(retval, value);
	val_type = zend_jit_load_type(llvm_ctx, value, -1, MAY_BE_ANY);
	bb_indirect = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	zend_jit_unexpected_br(llvm_ctx,
			llvm_ctx.builder.CreateICmpEQ(
				val_type,
				llvm_ctx.builder.getInt8(IS_INDIRECT)),
			bb_indirect,
			bb_follow);
	llvm_ctx.builder.SetInsertPoint(bb_indirect);
	value = zend_jit_load_ind(llvm_ctx, value);
	PHI_ADD(retval, value);
	val_type = zend_jit_load_type(llvm_ctx, value, -1, MAY_BE_ANY);
	bb_undef = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	zend_jit_unexpected_br(llvm_ctx,
			llvm_ctx.builder.CreateICmpEQ(
				val_type,
				llvm_ctx.builder.getInt8(IS_UNDEF)),
			bb_undef,
			bb_follow);
	llvm_ctx.builder.SetInsertPoint(bb_undef);
	zend_jit_save_zval_type_info(llvm_ctx, value, -1, MAY_BE_ANY, llvm_ctx.builder.getInt32(IS_NULL));
	PHI_ADD(retval, value);
	llvm_ctx.builder.CreateBr(bb_follow);

	llvm_ctx.builder.SetInsertPoint(bb_add_new);
	value = zend_jit_hash_update(llvm_ctx,
			symbol_table,
			llvm_ctx.builder.CreateIntToPtr(
				LLVM_GET_LONG((zend_uintptr_t)Z_STR_P(varname)),
				PointerType::getUnqual(llvm_ctx.zend_string_type)),
			llvm_ctx._EG_uninitialized_zval,
			opline);
	arData = llvm_ctx.builder.CreateAlignedLoad(
			zend_jit_GEP(
				llvm_ctx,
				symbol_table,
				offsetof(HashTable, arData),
				PointerType::getUnqual(PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context)))), 4);
	idx = llvm_ctx.builder.CreateExactSDiv(
			llvm_ctx.builder.CreateSub(
				llvm_ctx.builder.CreatePtrToInt(value, Type::getInt32Ty(llvm_ctx.context)),
				llvm_ctx.builder.CreatePtrToInt(arData,Type::getInt32Ty(llvm_ctx.context))),
			llvm_ctx.builder.getInt32(sizeof(Bucket)));
	llvm_ctx.builder.CreateAlignedStore(
			llvm_ctx.builder.CreateAdd(idx, llvm_ctx.builder.getInt32(1)),
				llvm_ctx.builder.CreateBitCast(
					cache_slot_addr,
					PointerType::getUnqual(Type::getInt32Ty(llvm_ctx.context))), 4);
	PHI_ADD(retval, value);
	llvm_ctx.builder.CreateBr(bb_follow);
	llvm_ctx.builder.SetInsertPoint(bb_follow);
	PHI_SET(retval, value, llvm_ctx.zval_ptr_type);

	variable = zend_jit_load_cv(llvm_ctx, OP1_OP()->var, OP1_INFO(), OP1_SSA_VAR(), 0, opline, BP_VAR_W);
	//JIT: zend_assign_to_variable_reference?
	if (OP1_MAY_BE(MAY_BE_REF)) {
		bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		bb_not_same = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		bb_next = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		zend_jit_expected_br(llvm_ctx,
			llvm_ctx.builder.CreateICmpNE(
				variable,
				value),
			bb_not_same,
			bb_next);
		llvm_ctx.builder.SetInsertPoint(bb_not_same);
	}

	zend_jit_make_ref(llvm_ctx, value, NULL, -1, MAY_BE_ANY|MAY_BE_REF);
	val_counted = zend_jit_load_counted(llvm_ctx, value, -1, MAY_BE_ANY);
	zend_jit_addref(llvm_ctx, val_counted);
	zend_jit_zval_ptr_dtor_ex(llvm_ctx, variable, NULL, OP1_SSA_VAR(), OP1_INFO(), opline->lineno, 1);
	zend_jit_save_zval_ptr(llvm_ctx,
			variable,
			OP1_SSA_VAR(),
			OP1_INFO(),
			val_counted);
	zend_jit_save_zval_type_info(llvm_ctx,
			variable, OP1_SSA_VAR(), OP1_INFO() ,llvm_ctx.builder.getInt32(IS_REFERENCE_EX));
	if (OP1_MAY_BE(MAY_BE_REF)) {
		llvm_ctx.builder.CreateBr(bb_follow);
		llvm_ctx.builder.SetInsertPoint(bb_next);
		zend_jit_make_ref(llvm_ctx, variable, NULL, OP1_SSA_VAR(), OP1_INFO());
		llvm_ctx.builder.CreateBr(bb_follow);
		llvm_ctx.builder.SetInsertPoint(bb_follow);
	}

	if (OP1_MAY_BE(MAY_BE_DEF)) {
		JIT_CHECK(zend_jit_check_exception(llvm_ctx, opline));
	}
	llvm_ctx.valid_opline = 0;
	return 1;
}
/* }}} */

/* PUBLIC API */

/* {{{ zend_jit_vm_stack_extend */
static Value* zend_jit_vm_stack_extend(zend_llvm_ctx    &llvm_ctx,
                                    Value            *size)
{
	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)zend_vm_stack_extend,
			ZEND_JIT_SYM("zend_vm_stack_extend"),
			0,
			LLVM_GET_LONG_TY(llvm_ctx.context),
			LLVM_GET_LONG_TY(llvm_ctx.context),
			NULL,
			NULL,
			NULL,
			NULL);

	return llvm_ctx.builder.CreateCall(_helper, size);
}
/* }}} */

/* {{{ zend_jit_vm_stack_alloc */
static Value* zend_jit_vm_stack_alloc(zend_llvm_ctx    &llvm_ctx,
                                      Value            *size,
                                      uint32_t          lineno)
{
	// JIT: char *top = (char*)EG(vm_stack_top);
	Value *top = llvm_ctx.builder.CreateAlignedLoad(
			llvm_ctx._EG_vm_stack_top, 4);
	PHI_DCL(ret, 2);

	//JIT: if (UNEXPECTED(size > (size_t)(((char*)EG(vm_stack_end)) - top))) {
	BasicBlock *bb_extend = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	BasicBlock *bb_add = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	BasicBlock *bb_common = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	zend_jit_unexpected_br(llvm_ctx,
		llvm_ctx.builder.CreateICmpUGT(
				size,
				llvm_ctx.builder.CreateSub(
					llvm_ctx.builder.CreateAlignedLoad(
						llvm_ctx._EG_vm_stack_end, 4),
					top)),
		bb_extend,
		bb_add);
	llvm_ctx.builder.SetInsertPoint(bb_extend);
	//JIT: return zend_vm_stack_extend(size TSRMLS_CC);
	Value *ret = zend_jit_vm_stack_extend(llvm_ctx, size);
	PHI_ADD(ret, ret);
	llvm_ctx.builder.CreateBr(bb_common);

	llvm_ctx.builder.SetInsertPoint(bb_add);
	//JIT: EG(vm_stack_top) = (zval*)(top + size);
	llvm_ctx.builder.CreateAlignedStore(
		llvm_ctx.builder.CreateAdd(
			top,
			size),
		llvm_ctx._EG_vm_stack_top, 4);	
	PHI_ADD(ret, top);
	llvm_ctx.builder.CreateBr(bb_common);

	llvm_ctx.builder.SetInsertPoint(bb_common);

	PHI_SET(ret, ret, LLVM_GET_LONG_TY(llvm_ctx.context));
	//JIT: return (zval*)top;
	return llvm_ctx.builder.CreateIntToPtr(ret,
			PointerType::getUnqual(llvm_ctx.zend_execute_data_type));
}
/* }}} */

/* {{{ static int zend_jit_vm_stack_push_call_frame_ex */
static Value* zend_jit_vm_stack_push_call_frame_ex(zend_llvm_ctx    &llvm_ctx,
                                                   zend_function    *func,
                                                   uint32_t          num_args,
                                                   Value            *used_stack,
                                                   Value            *func_addr,
                                                   uint32_t          lineno)
{
	//JIT: call = (zend_execute_data*)zend_vm_stack_alloc(used_stack);
	Value *call = zend_jit_vm_stack_alloc(llvm_ctx,
		used_stack,
		lineno);
	//JIT: call->func = func;
	llvm_ctx.builder.CreateAlignedStore(
		func_addr,
		zend_jit_GEP(
			llvm_ctx,
			call,
			offsetof(zend_execute_data, func),
			PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.zend_function_type))), 4);
	//JIT: Z_OBJ(call->This) = object;
	llvm_ctx.builder.CreateAlignedStore(
		LLVM_GET_LONG(0), //???
		zend_jit_GEP(
			llvm_ctx,
			call,
			offsetof(zend_execute_data, This.value.obj),
			PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4);
	//JIT: ZEND_SET_CALL_INFO(call, call_info);
	llvm_ctx.builder.CreateAlignedStore(
		llvm_ctx.builder.getInt32(IS_OBJECT_EX | (ZEND_CALL_TOP_FUNCTION << 24)),
		zend_jit_GEP(
			llvm_ctx,
			call,
			offsetof(zend_execute_data, This.u1.type_info),
			PointerType::getUnqual(Type::getInt32Ty(llvm_ctx.context))), 4);
	//JIT: ZEND_CALL_NUM_ARGS(call) = num_args;
	llvm_ctx.builder.CreateAlignedStore(
		llvm_ctx.builder.getInt32(num_args),
		zend_jit_GEP(
			llvm_ctx,
			call,
			offsetof(zend_execute_data, This.u2.num_args),
			PointerType::getUnqual(Type::getInt32Ty(llvm_ctx.context))), 4);
	//JIT: call->called_scope = called_scope;
	llvm_ctx.builder.CreateAlignedStore(
		LLVM_GET_LONG(0), //???
		zend_jit_GEP(
			llvm_ctx,
			call,
			offsetof(zend_execute_data, called_scope),
			PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4);
	//JIT: call->prev_execute_data = prev;
	Value *prev = NULL;
	if (llvm_ctx.call_level == 1) {
		prev = llvm_ctx.builder.CreateIntToPtr(
			LLVM_GET_LONG(0),
			PointerType::getUnqual(llvm_ctx.zend_execute_data_type));
	} else {
		prev = llvm_ctx.builder.CreateAlignedLoad(
			zend_jit_GEP(
				llvm_ctx,
				llvm_ctx._execute_data,
				offsetof(zend_execute_data, call),
				PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.zend_execute_data_type))), 4);
	}
	llvm_ctx.builder.CreateAlignedStore(
		prev,
		zend_jit_GEP(
			llvm_ctx,
			call,
			offsetof(zend_execute_data, prev_execute_data),
			PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.zend_execute_data_type))), 4);
	
	return call;
}
/* }}} */

/* {{{ static int zend_jit_vm_stack_push_call_frame */
static Value* zend_jit_vm_stack_push_call_frame(zend_llvm_ctx    &llvm_ctx,
                                                zend_function    *func,
                                                uint32_t          num_args,
                                                Value            *func_addr,
                                                uint32_t          lineno)
{
	Value *used_stack_val = NULL;
	
	if (func) {
		used_stack_val = LLVM_GET_LONG(zend_vm_calc_used_stack(num_args, func));
	} else {
		used_stack_val = LLVM_GET_LONG(ZEND_CALL_FRAME_SLOT + num_args);
		PHI_DCL(used_stack_val, 3);
		PHI_ADD(used_stack_val, used_stack_val);

		//JIT: if (ZEND_USER_CODE(func->type)) {
		BasicBlock *bb_user = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_common = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		zend_jit_expected_br(llvm_ctx,
			llvm_ctx.builder.CreateICmpEQ(
				llvm_ctx.builder.CreateAnd(
					llvm_ctx.builder.CreateAlignedLoad(
						zend_jit_GEP(
							llvm_ctx,
							func_addr,
							offsetof(zend_function, type),
							PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context))), 1),
					llvm_ctx.builder.getInt8(1)),
				llvm_ctx.builder.getInt8(0)),
			bb_user,
			bb_common);
		llvm_ctx.builder.SetInsertPoint(bb_user);
		//JIT: used_stack += func->op_array.last_var + func->op_array.T - MIN(func->op_array.num_args, num_args);
		//JIT: used_stack += func->op_array.last_var + func->op_array.T - num_args + ((num_args - func->op_array.num_args) > 0 ? num_args - func->op_array.num_args : 0);
		used_stack_val = llvm_ctx.builder.CreateAdd(
			llvm_ctx.builder.CreateAdd(			
				LLVM_GET_LONG(ZEND_CALL_FRAME_SLOT),
				llvm_ctx.builder.CreateZExtOrBitCast(
					llvm_ctx.builder.CreateAlignedLoad(
						zend_jit_GEP(
							llvm_ctx,
							func_addr,
							offsetof(zend_function, op_array.last_var),
							PointerType::getUnqual(Type::getInt32Ty(llvm_ctx.context))), 4),
					LLVM_GET_LONG_TY(llvm_ctx.context))),
			llvm_ctx.builder.CreateZExtOrBitCast(
				llvm_ctx.builder.CreateAlignedLoad(
					zend_jit_GEP(
						llvm_ctx,
						func_addr,
						offsetof(zend_function, op_array.T),
						PointerType::getUnqual(Type::getInt32Ty(llvm_ctx.context))), 4),
				LLVM_GET_LONG_TY(llvm_ctx.context)));

		Value *diff = llvm_ctx.builder.CreateSub(
			llvm_ctx.builder.getInt32(num_args),
			llvm_ctx.builder.CreateAlignedLoad(
				zend_jit_GEP(
					llvm_ctx,
					func_addr,
					offsetof(zend_function, op_array.num_args),
					PointerType::getUnqual(Type::getInt32Ty(llvm_ctx.context))), 4));
		PHI_ADD(used_stack_val, used_stack_val);

		BasicBlock *bb_more = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		zend_jit_expected_br(llvm_ctx,
			llvm_ctx.builder.CreateICmpSGT(
				diff,
				llvm_ctx.builder.getInt32(0)),
			bb_more,
			bb_common);
		llvm_ctx.builder.SetInsertPoint(bb_more);

		used_stack_val = llvm_ctx.builder.CreateAdd(			
			used_stack_val,
			llvm_ctx.builder.CreateZExtOrBitCast(
				diff,
				LLVM_GET_LONG_TY(llvm_ctx.context)));
		PHI_ADD(used_stack_val, used_stack_val);

		llvm_ctx.builder.CreateBr(bb_common);
		llvm_ctx.builder.SetInsertPoint(bb_common);
		PHI_SET(used_stack_val, used_stack_val, LLVM_GET_LONG_TY(llvm_ctx.context));
	}

	return zend_jit_vm_stack_push_call_frame_ex(llvm_ctx, func, num_args,
		llvm_ctx.builder.CreateMul(
			used_stack_val,
			LLVM_GET_LONG(sizeof(zval))),
        func_addr,
        lineno);
}
/* }}} */

/* {{{ static int zend_jit_init_fcall */
static int zend_jit_init_fcall(zend_llvm_ctx    &llvm_ctx,
                               zend_jit_context *ctx,
                               zend_op_array    *op_array,
                               zend_op          *opline)
{
	zend_jit_func_info *info = JIT_DATA(op_array);
	zend_jit_call_info *call_info = info->callee_info;
	zend_function *func = NULL;
	Value *func_addr = NULL;

	while (call_info && call_info->caller_init_opline != opline) {
		call_info = call_info->next_callee;
	}
	if (call_info && call_info->callee_func) {
		func = call_info->callee_func;
	}

	if (func && func->type == ZEND_INTERNAL_FUNCTION) {
		func_addr = llvm_ctx.builder.CreateIntToPtr(
				LLVM_GET_LONG((zend_uintptr_t)func),
				PointerType::getUnqual(llvm_ctx.zend_function_type));
	} else if (func && func->type == ZEND_USER_FUNCTION && op_array == &func->op_array) {
		func_addr = llvm_ctx.builder.CreateAlignedLoad(
			zend_jit_GEP(
				llvm_ctx,
				llvm_ctx._execute_data,
				offsetof(zend_execute_data, func),
				PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.zend_function_type))), 4);
	} else {
		BasicBlock *bb_not_cached = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_common = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		PHI_DCL(func_addr, 2);

		//JIT: if (CACHED_PTR(Z_CACHE_SLOT_P(fname))) {
		Value *cache_slot_addr = zend_jit_cache_slot_addr(
				llvm_ctx,
				Z_CACHE_SLOT_P(RT_CONSTANT(llvm_ctx.op_array, *OP2_OP())),
				PointerType::getUnqual(llvm_ctx.zend_function_type));
		func_addr = llvm_ctx.builder.CreateAlignedLoad(cache_slot_addr, 4);
		PHI_ADD(func_addr, func_addr);
		zend_jit_unexpected_br(llvm_ctx,
			llvm_ctx.builder.CreateIsNull(func_addr),
			bb_not_cached,
			bb_common);
		llvm_ctx.builder.SetInsertPoint(bb_not_cached);
		//JIT: } else if (UNEXPECTED((func = zend_hash_find(EG(function_table), Z_STR_P(fname))) == NULL)) {
		Value *zv_addr = zend_jit_hash_find(llvm_ctx,
			llvm_ctx.builder.CreateAlignedLoad(
				llvm_ctx._EG_function_table, 4),
			llvm_ctx.builder.CreateIntToPtr(
				(opline->opcode == ZEND_INIT_FCALL) ?
					LLVM_GET_LONG((zend_uintptr_t)(Z_STR_P(RT_CONSTANT(llvm_ctx.op_array, *OP2_OP())))) :
					LLVM_GET_LONG((zend_uintptr_t)(Z_STR_P(RT_CONSTANT(llvm_ctx.op_array, *OP2_OP()) + 1))),
				PointerType::getUnqual(llvm_ctx.zend_string_type)));

		if (!func) {
			BasicBlock *bb_not_found  = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			BasicBlock *bb_found      = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

			zend_jit_unexpected_br(llvm_ctx,
				llvm_ctx.builder.CreateIsNull(zv_addr),
				bb_not_found,
				bb_found);
			llvm_ctx.builder.SetInsertPoint(bb_not_found);
			//JIT: SAVE_OPLINE();
			if (!llvm_ctx.valid_opline) {
				JIT_CHECK(zend_jit_store_opline(llvm_ctx, opline, false));
			}
			//JIT: zend_throw_error(NULL, "Call to undefined function %s()", Z_STRVAL_P(fname));
			zend_jit_throw_error(
				llvm_ctx,
				opline,
				NULL,
				"Call to undefined function %s()",
				LLVM_GET_CONST_STRING(Z_STRVAL_P(RT_CONSTANT(llvm_ctx.op_array, *OP2_OP()))));
			//JIT: HANDLE_EXCEPTION();
			llvm_ctx.builder.CreateBr(zend_jit_find_exception_bb(llvm_ctx, opline));
			//JIT: } else {
			llvm_ctx.builder.SetInsertPoint(bb_found);
		}
		//JIT: fbc = Z_FUNC_P(func);
		func_addr = llvm_ctx.builder.CreateAlignedLoad(
			zend_jit_GEP(
				llvm_ctx,
				zv_addr,
				offsetof(zval, value.ptr),
				PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.zend_function_type))), 4);
		//JIT: CACHE_PTR(Z_CACHE_SLOT_P(fname), fbc);
		llvm_ctx.builder.CreateAlignedStore(
			func_addr,
			cache_slot_addr, 4);
		if (bb_common) {
			PHI_ADD(func_addr, func_addr);
			llvm_ctx.builder.CreateBr(bb_common);
			llvm_ctx.builder.SetInsertPoint(bb_common);
			PHI_SET(func_addr, func_addr, PointerType::getUnqual(llvm_ctx.zend_function_type));
		} 
	}

	//JIT: EX(call) = zend_vm_stack_push_call_frame(fbc, opline->extended_value, 0, NULL, NULL, EX(call) TSRMLS_CC);
	llvm_ctx.builder.CreateAlignedStore(
		zend_jit_vm_stack_push_call_frame_ex(llvm_ctx,
			func,
			opline->extended_value,
			LLVM_GET_LONG(opline->op1.num),
			func_addr,
			opline->lineno),
		zend_jit_GEP(
			llvm_ctx,
			llvm_ctx._execute_data,
			offsetof(zend_execute_data, call),
			PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.zend_execute_data_type))), 4);

	//JIT: ZEND_VM_NEXT_OPCODE();
	llvm_ctx.valid_opline = 0;
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_vm_stack_free_args */
static int zend_jit_vm_stack_free_args(zend_llvm_ctx      &llvm_ctx,
                                       uint32_t            num_args,
                                       Value              *call,
                                       zend_op            *opline)
{
	if (num_args != -1) {
		while (num_args > 0) {
			num_args--;
			//JIT: zval *arg = ZEND_CALL_ARG(call, num_args);
			Value *arg = zend_jit_GEP(
				llvm_ctx,
				call,
				(ZEND_CALL_FRAME_SLOT + num_args) * sizeof(zval),
				llvm_ctx.zval_ptr_type);
			//JIT: zval_ptr_dtor_nogc(p);
			zend_jit_zval_ptr_dtor_ex(llvm_ctx, arg, NULL, -1, MAY_BE_ANY, opline->lineno, 0);
		}
	} else {
//???..check
		Value *num_args_val =
			llvm_ctx.builder.CreateAlignedLoad(
				zend_jit_GEP(
					llvm_ctx,
					call,
					offsetof(zend_execute_data, This.u2.num_args),
					PointerType::getUnqual(Type::getInt32Ty(llvm_ctx.context))), 4);
		//JIT: if (EXPECTED(num_args < op_array->last_var)) {
		BasicBlock *bb_init = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_end = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_loop = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		zend_jit_expected_br(llvm_ctx,
			llvm_ctx.builder.CreateICmpUGT(
				num_args_val,
				llvm_ctx.builder.getInt32(0)),
			bb_init,
			bb_end);
		llvm_ctx.builder.SetInsertPoint(bb_init);
		//JIT: zval *end = ZEND_CALL_ARG(call, 1);
		Value *end = zend_jit_GEP(
			llvm_ctx,
			call,
			ZEND_CALL_FRAME_SLOT * sizeof(zval),
			llvm_ctx.zval_ptr_type);
		//JIT: zval *p = end + num_args;
		Value *var = llvm_ctx.builder.CreateInBoundsGEP(
			end,
			num_args_val);
		//JIT: do {
		PHI_DCL2(var, 2);
		PHI_ADD2(var, var);
		llvm_ctx.builder.CreateBr(bb_loop);
		llvm_ctx.builder.SetInsertPoint(bb_loop);
		PHI_SET2(var, var, llvm_ctx.zval_ptr_type);
		//JIT: var--;
		var = zend_jit_GEP(
			llvm_ctx,
			var,
			-sizeof(zval),
			llvm_ctx.zval_ptr_type);
		//JIT: zval_ptr_dtor_nogc(p);
		zend_jit_zval_ptr_dtor_ex(llvm_ctx, var, NULL, -1, MAY_BE_ANY, opline->lineno, 0);
		//JIT: } while (var != end);
		PHI_ADD2(var, var);
		zend_jit_expected_br(llvm_ctx,
			llvm_ctx.builder.CreateICmpNE(
				var,
				end),
			bb_loop,
			bb_end);
		llvm_ctx.builder.SetInsertPoint(bb_end);
	}
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_free_extra_args */
static int zend_jit_free_call_frame_helper(zend_llvm_ctx &llvm_ctx)
{
	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)zend_jit_helper_free_call_frame,
			ZEND_JIT_SYM("zend_jit_helper_free_call_frame"),
			ZEND_JIT_HELPER_FAST_CALL,
			Type::getVoidTy(llvm_ctx.context),
			NULL,
			NULL,
			NULL,
			NULL,
			NULL);

	CallInst *call = llvm_ctx.builder.CreateCall(_helper);
	call->setCallingConv(CallingConv::X86_FastCall);
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_vm_stack_free_call_frame */
static int zend_jit_vm_stack_free_call_frame(zend_llvm_ctx    &llvm_ctx,
                                             Value            *call,
                                             uint32_t          lineno)
{
	//JIT: zend_vm_stack p = EG(vm_stack);
	Value *p = llvm_ctx.builder.CreateAlignedLoad(llvm_ctx._EG_vm_stack, 4);
	//JIT: if (UNEXPECTED(ZEND_VM_STACK_ELEMETS(p) == (zval*)call)) {
	BasicBlock *bb_fast = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	BasicBlock *bb_slow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	BasicBlock *bb_common = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	zend_jit_unexpected_br(llvm_ctx,
		llvm_ctx.builder.CreateICmpEQ(
			zend_jit_GEP(
				llvm_ctx,
				p,
				ZEND_VM_STACK_HEADER_SLOTS * sizeof(zval),
				PointerType::getUnqual(llvm_ctx.zend_execute_data_type)),
			call),
		bb_slow,
		bb_fast);
	llvm_ctx.builder.SetInsertPoint(bb_slow);
#if 1
	zend_jit_free_call_frame_helper(llvm_ctx);
#else
	//JIT: zend_vm_stack prev = p->prev;
	Value *prev = llvm_ctx.builder.CreateAlignedLoad(
			zend_jit_GEP(
				llvm_ctx,
				p,
				offsetof(struct _zend_vm_stack, prev),
				PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.zend_vm_stack_type))), 4);
	//JIT: EG(vm_stack_top) = prev->top;
	llvm_ctx.builder.CreateAlignedStore(
		llvm_ctx.builder.CreateAlignedLoad(
			zend_jit_GEP(
				llvm_ctx,
				prev,
				offsetof(struct _zend_vm_stack, top),
				PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4),
		llvm_ctx._EG_vm_stack_top, 4);
	//JIT: EG(vm_stack_end) = prev->end;
	llvm_ctx.builder.CreateAlignedStore(
		llvm_ctx.builder.CreateAlignedLoad(
			zend_jit_GEP(
				llvm_ctx,
				prev,
				offsetof(struct _zend_vm_stack, end),
				PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4),
		llvm_ctx._EG_vm_stack_end, 4);
	//JIT: EG(vm_stack) = prev;
	llvm_ctx.builder.CreateAlignedStore(
		prev,
		llvm_ctx._EG_vm_stack, 4);
	//JIT: efree(p);
	zend_jit_efree(llvm_ctx, p, lineno);
	//JIT: p->top = (zval*)call;
#endif
	llvm_ctx.builder.CreateBr(bb_common);	
	llvm_ctx.builder.SetInsertPoint(bb_fast);
	llvm_ctx.builder.CreateAlignedStore(
		llvm_ctx.builder.CreatePtrToInt(
			call,
			LLVM_GET_LONG_TY(llvm_ctx.context)),
		llvm_ctx._EG_vm_stack_top, 4);
	llvm_ctx.builder.CreateBr(bb_common);	
	llvm_ctx.builder.SetInsertPoint(bb_common);	
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_object_store_ctor_failed */
static int zend_jit_object_store_ctor_failed(zend_llvm_ctx    &llvm_ctx,
                                             Value            *object)
{
//???..check
	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)zend_object_store_ctor_failed,
			ZEND_JIT_SYM("zend_object_store_ctor_failed"),
			0,
			Type::getVoidTy(llvm_ctx.context),
			PointerType::getUnqual(llvm_ctx.zend_object_type),
			NULL,
			NULL,
			NULL,
			NULL);

	llvm_ctx.builder.CreateCall(
		_helper, object);
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_throw_exception_internal */
static int zend_jit_throw_exception_internal(zend_llvm_ctx    &llvm_ctx,
                                             Value            *object)
{
//???..check
	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)zend_throw_exception_internal,
			ZEND_JIT_SYM("zend_throw_exception_internal"),
			0,
			Type::getVoidTy(llvm_ctx.context),
			llvm_ctx.zval_ptr_type,
			NULL,
			NULL,
			NULL,
			NULL);

	llvm_ctx.builder.CreateCall(
		_helper, object);
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_arena_alloc */
static Value* zend_jit_arena_alloc(zend_llvm_ctx    &llvm_ctx,
                                    Value            *arena_ptr,
                                    Value            *size,
                                    uint32_t          lineno)
{
	BasicBlock *bb_fast = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	BasicBlock *bb_slow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	BasicBlock *bb_common = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	//JIT: zend_arena *arena = *arena_ptr;
	Value *arena = llvm_ctx.builder.CreateAlignedLoad(arena_ptr, 4);
	//JIT: char *ptr = arena->ptr;
	Value *ptr = llvm_ctx.builder.CreateAlignedLoad(
		zend_jit_GEP(
			llvm_ctx,
			arena,
			offsetof(zend_arena, ptr),
			PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4);
	PHI_DCL(ptr, 2);

	//JIT: size = ZEND_MM_ALIGNED_SIZE(size);
	size = llvm_ctx.builder.CreateAnd(
		llvm_ctx.builder.CreateAdd(
			size,
			LLVM_GET_LONG(ZEND_MM_ALIGNMENT - 1)),
		LLVM_GET_LONG(~(ZEND_MM_ALIGNMENT - 1)));

	//JIT: if (EXPECTED(size <= (size_t)(arena->end - ptr))) {
	zend_jit_expected_br(llvm_ctx,
		llvm_ctx.builder.CreateICmpULE(
			size,
			llvm_ctx.builder.CreateSub(
				llvm_ctx.builder.CreateAlignedLoad(
					zend_jit_GEP(
						llvm_ctx,
						arena,
						offsetof(zend_arena, end),
						PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4),
				ptr)),
		bb_fast,
		bb_slow);
	llvm_ctx.builder.SetInsertPoint(bb_fast);
	//JIT: arena->ptr = ptr + size;
	llvm_ctx.builder.CreateAlignedStore(
		llvm_ctx.builder.CreateAdd(
			ptr,
			size),
		zend_jit_GEP(
			llvm_ctx,
			arena,
			offsetof(zend_arena, ptr),
			PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4);
	PHI_ADD(ptr, ptr);
	llvm_ctx.builder.CreateBr(bb_common);

	llvm_ctx.builder.SetInsertPoint(bb_slow);
	//JIT: size_t arena_size = 
	//       UNEXPECTED((size + ZEND_MM_ALIGNED_SIZE(sizeof(zend_arena))) > (size_t)(arena->end - (char*) arena)) ?
	//       (size + ZEND_MM_ALIGNED_SIZE(sizeof(zend_arena))) :
	//       (size_t)(arena->end - (char*) arena);
	Value *arena_size = LLVM_GET_LONG(64 * 1024);
	//???... proper size
	//JIT: zend_arena *new_arena = (zend_arena*)emalloc(arena_size);
	Value *new_arena = zend_jit_emalloc(llvm_ctx, arena_size, lineno);
	//JIT: ptr = (char*) new_arena + ZEND_MM_ALIGNED_SIZE(sizeof(zend_arena));
	ptr = llvm_ctx.builder.CreateAdd(
			llvm_ctx.builder.CreatePtrToInt(
				new_arena,
				LLVM_GET_LONG_TY(llvm_ctx.context)),
			LLVM_GET_LONG(sizeof(zend_arena)));
	//JIT: new_arena->ptr = (char*) ptr + size;
	llvm_ctx.builder.CreateAlignedStore(
		llvm_ctx.builder.CreateAdd(
			ptr,
			size),
		zend_jit_GEP(
			llvm_ctx,
			new_arena,
			offsetof(zend_arena, ptr),
			PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4);
	//JIT: new_arena->end = (char*) new_arena + arena_size;
	llvm_ctx.builder.CreateAlignedStore(
		llvm_ctx.builder.CreateAdd(
			llvm_ctx.builder.CreatePtrToInt(
				new_arena,
				LLVM_GET_LONG_TY(llvm_ctx.context)),
			arena_size),
		zend_jit_GEP(
			llvm_ctx,
			new_arena,
			offsetof(zend_arena, end),
			PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4);
	//JIT: new_arena->prev = arena;
	llvm_ctx.builder.CreateAlignedStore(
		arena,
		zend_jit_GEP(
			llvm_ctx,
			new_arena,
			offsetof(zend_arena, prev),
			PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.zend_arena_type))), 4);
	//JIT: *arena_ptr = new_arena;				
	llvm_ctx.builder.CreateAlignedStore(
		llvm_ctx.builder.CreateBitCast(
			new_arena,
			PointerType::getUnqual(llvm_ctx.zend_arena_type)),
		arena_ptr, 4);
	PHI_ADD(ptr, ptr);
	llvm_ctx.builder.CreateBr(bb_common);

	//JIT: return (void*) ptr;
	llvm_ctx.builder.SetInsertPoint(bb_common);	
	PHI_SET(ptr, ptr, LLVM_GET_LONG_TY(llvm_ctx.context));
	return llvm_ctx.builder.CreateIntToPtr(
			ptr,
			PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context)));
}
/* }}} */

/* {{{ static int zend_jit_arena_calloc */
static Value* zend_jit_arena_calloc(zend_llvm_ctx    &llvm_ctx,
                                    Value            *arena_ptr,
                                    Value            *num,
                                    size_t            size,
                                    uint32_t          lineno)
{
	Value *msize = llvm_ctx.builder.CreateMul(
		LLVM_GET_LONG(size),
		llvm_ctx.builder.CreateZExtOrBitCast(
			num,
			LLVM_GET_LONG_TY(llvm_ctx.context)));
#if 1
	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)zend_jit_helper_arena_calloc,
			ZEND_JIT_SYM("zend_jit_helper_arena_caloc"),
			ZEND_JIT_HELPER_FAST_CALL,
			PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context)),
			LLVM_GET_LONG_TY(llvm_ctx.context),
			NULL,
			NULL,
			NULL,
			NULL);

	CallInst *call = llvm_ctx.builder.CreateCall(_helper, msize);
	call->setCallingConv(CallingConv::X86_FastCall);
	return call;
#else
	Value *ret = zend_jit_arena_alloc(llvm_ctx, arena_ptr, msize, lineno);
	//JIT: memset(ret, 0, size);
	llvm_ctx.builder.CreateMemSet(
		ret,
		llvm_ctx.builder.getInt8(0),
		msize,
		4);
	return ret;
#endif
}
/* }}} */

/* {{{ static int zend_jit_init_func_execute_data */
static int zend_jit_init_func_execute_data(zend_llvm_ctx    &llvm_ctx,
                                           Value            *execute_data,
                                           Value            *object,
                                           zend_function    *func,
                                           Value            *func_addr,
                                           Value            *return_value,
                                           uint32_t          num_args,
                                           uint32_t          lineno)
{
	//JIT: opline = op_array->opcodes;
	Value *opline = NULL;
	if (func) {
		opline = LLVM_GET_LONG((zend_uintptr_t)func->op_array.opcodes);
	} else {
		opline = llvm_ctx.builder.CreateAlignedLoad(
			zend_jit_GEP(
				llvm_ctx,
				func_addr,
				offsetof(zend_function, op_array.opcodes),
				PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4);
	}
	//JIT: EX(call) = NULL;
	llvm_ctx.builder.CreateAlignedStore(
		llvm_ctx.builder.CreateIntToPtr(
			LLVM_GET_LONG(0),
			PointerType::getUnqual(llvm_ctx.zend_execute_data_type)),
		zend_jit_GEP(
			llvm_ctx,
			execute_data,
			offsetof(zend_execute_data, call),
			PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.zend_execute_data_type))), 4);
	//JIT: EX(return_value) = return_value;
	llvm_ctx.builder.CreateAlignedStore(
		return_value,
		zend_jit_GEP(
			llvm_ctx,
			execute_data,
			offsetof(zend_execute_data, return_value),
			PointerType::getUnqual(llvm_ctx.zval_ptr_type)), 4);

#if 1
	/* Handle arguments */
	if (func && num_args != -1) {
		uint32_t first_extra_arg = func->op_array.num_args;
		if (num_args > first_extra_arg) {
			if (EXPECTED((func->op_array.fn_flags & ZEND_ACC_HAS_TYPE_HINTS) == 0)) {
				/* Skip useless ZEND_RECV and ZEND_RECV_INIT opcodes */
				//JIT: EX(opline) = opline + first_extra_arg;
				llvm_ctx.builder.CreateAlignedStore(
					llvm_ctx.builder.CreateAdd(
						opline,
						LLVM_GET_LONG(first_extra_arg * sizeof(zend_op))),
					zend_jit_GEP(
						llvm_ctx,
						execute_data,
						offsetof(zend_execute_data, opline),
						PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4);
			} else {
				//JIT: EX(opline) = opline;
				llvm_ctx.builder.CreateAlignedStore(
					opline,
					zend_jit_GEP(
						llvm_ctx,
						execute_data,
						offsetof(zend_execute_data, opline),
						PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4);
			}

			/* move extra args into separate array after all CV and TMP vars */
			uint32_t end = ZEND_CALL_FRAME_SLOT + first_extra_arg - 1;
			uint32_t src = end + (num_args - first_extra_arg);
			uint32_t dst = src + (func->op_array.last_var + func->op_array.T - first_extra_arg);
			if (src != dst) {
				do {
					Value *dst_val = zend_jit_GEP(
						llvm_ctx,
						execute_data,
						dst * sizeof(zval),
						llvm_ctx.zval_ptr_type);
					Value *src_val = zend_jit_GEP(
						llvm_ctx,
						execute_data,
						src * sizeof(zval),
						llvm_ctx.zval_ptr_type);
					//JIT: ZVAL_COPY_VALUE(dst, src);
					zend_jit_copy_value(llvm_ctx, 
						dst_val,
						0,
						-1,
						MAY_BE_ANY,
						src_val,
						NULL, 
						IS_VAR,
						NULL,
						-1,
						MAY_BE_ANY);						
					//JIT: ZVAL_UNDEF(src);
					zend_jit_save_zval_type_info(llvm_ctx, src_val, -1, MAY_BE_ANY, llvm_ctx.builder.getInt32(IS_UNDEF));
					src--;
					dst--;
				} while (src != end);
			}
		} else if (EXPECTED((func->op_array.fn_flags & ZEND_ACC_HAS_TYPE_HINTS) == 0)) {
			/* Skip useless ZEND_RECV and ZEND_RECV_INIT opcodes */
			//JIT: EX(opline) = opline + num_args;
			llvm_ctx.builder.CreateAlignedStore(
				llvm_ctx.builder.CreateAdd(
					opline,
					LLVM_GET_LONG(num_args * sizeof(zend_op))),
				zend_jit_GEP(
					llvm_ctx,
					execute_data,
					offsetof(zend_execute_data, opline),
					PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4);
        } else {
			//JIT: EX(opline) = opline;
			llvm_ctx.builder.CreateAlignedStore(
				opline,
				zend_jit_GEP(
					llvm_ctx,
					execute_data,
					offsetof(zend_execute_data, opline),
					PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4);
		}
	} else {
		/* Handle arguments */
		Value *first_extra_arg_val = NULL;
		Value *num_args_val = NULL;

		if (func) {
			uint32_t first_extra_arg = func->op_array.num_args;
			first_extra_arg_val = llvm_ctx.builder.getInt32(first_extra_arg);
		} else {
			//JIT: first_extra_arg = op_array->num_args;
			first_extra_arg_val = 
				llvm_ctx.builder.CreateAlignedLoad(
					zend_jit_GEP(
						llvm_ctx,
						func_addr,
						offsetof(zend_function, op_array.num_args),
						PointerType::getUnqual(Type::getInt32Ty(llvm_ctx.context))), 4);
		}
		if (num_args != -1) {
			num_args_val = llvm_ctx.builder.getInt32(num_args);
		} else {
			//JIT: num_args = EX_NUM_ARGS();
			num_args_val = 
				llvm_ctx.builder.CreateAlignedLoad(
					zend_jit_GEP(
						llvm_ctx,
						execute_data,
						offsetof(zend_execute_data, This.u2.num_args),
						PointerType::getUnqual(Type::getInt32Ty(llvm_ctx.context))), 4);
		}

		//JIT: if (UNEXPECTED(num_args > first_extra_arg)) {
		BasicBlock *bb_extra = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_no_extra = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_end = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		zend_jit_unexpected_br(llvm_ctx,
			llvm_ctx.builder.CreateICmpUGT(
				num_args_val,
				first_extra_arg_val),
			bb_extra,
			bb_no_extra);
		llvm_ctx.builder.SetInsertPoint(bb_extra);
		if (func) {
			if (EXPECTED((func->op_array.fn_flags & ZEND_ACC_HAS_TYPE_HINTS) == 0)) {
				/* Skip useless ZEND_RECV and ZEND_RECV_INIT opcodes */
				//JIT: EX(opline) = opline + first_extra_arg;
				llvm_ctx.builder.CreateAlignedStore(
					llvm_ctx.builder.CreateAdd(
						opline,
						llvm_ctx.builder.CreateMul(
							llvm_ctx.builder.CreateZExtOrBitCast(
								first_extra_arg_val,
								LLVM_GET_LONG_TY(llvm_ctx.context)),
							LLVM_GET_LONG(sizeof(zend_op)))),
					zend_jit_GEP(
						llvm_ctx,
						execute_data,
						offsetof(zend_execute_data, opline),
						PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4);
	        } else {
				//JIT: EX(opline) = opline;
				llvm_ctx.builder.CreateAlignedStore(
					opline,
					zend_jit_GEP(
						llvm_ctx,
						execute_data,
						offsetof(zend_execute_data, opline),
						PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4);
			}
		} else {
			//JIT: if (EXPECTED((op_array->fn_flags & ZEND_ACC_HAS_TYPE_HINTS) == 0)) {
			//JIT: EX(opline) += first_extra_arg;
			llvm_ctx.builder.CreateAlignedStore(
				llvm_ctx.builder.CreateAdd(
					opline,
					llvm_ctx.builder.CreateZExtOrBitCast(
						llvm_ctx.builder.CreateMul(
							first_extra_arg_val,
							llvm_ctx.builder.CreateZExtOrBitCast(
								llvm_ctx.builder.CreateICmpEQ(
									llvm_ctx.builder.CreateAnd(
										llvm_ctx.builder.CreateAlignedLoad(
											zend_jit_GEP(
												llvm_ctx,
												func_addr,
												offsetof(zend_function, op_array.fn_flags),
												PointerType::getUnqual(Type::getInt32Ty(llvm_ctx.context))), 4),
										llvm_ctx.builder.getInt32(ZEND_ACC_HAS_TYPE_HINTS)),
									llvm_ctx.builder.getInt32(0)),
								Type::getInt32Ty(llvm_ctx.context))),
						LLVM_GET_LONG_TY(llvm_ctx.context))),
				zend_jit_GEP(
					llvm_ctx,
					execute_data,
					offsetof(zend_execute_data, opline),
					PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4);
        }
//???..check
		//JIT: end = EX_VAR_NUM(first_extra_arg - 1);
		Value *base = zend_jit_GEP(
			llvm_ctx,
			execute_data,
			ZEND_CALL_FRAME_SLOT * sizeof(zval),
			llvm_ctx.zval_ptr_type);
		Value *end = llvm_ctx.builder.CreateInBoundsGEP(
			base,
			llvm_ctx.builder.CreateSub(
				first_extra_arg_val,
				llvm_ctx.builder.getInt32(1)));
		//JIT: src = end + (num_args - first_extra_arg);
		Value *src = llvm_ctx.builder.CreateInBoundsGEP(
			end,
			llvm_ctx.builder.CreateSub(
				num_args_val,
				first_extra_arg_val));
		//JIT: dst = src + (op_array->last_var + op_array->T - first_extra_arg);
		Value *used_slots = NULL;
		if (func) {
			used_slots = llvm_ctx.builder.getInt32(func->op_array.last_var + func->op_array.T);
		} else {
			used_slots =
				llvm_ctx.builder.CreateAdd(
					llvm_ctx.builder.CreateAlignedLoad(
						zend_jit_GEP(
							llvm_ctx,
							func_addr,
							offsetof(zend_function, op_array.last_var),
							PointerType::getUnqual(Type::getInt32Ty(llvm_ctx.context))), 4),
					llvm_ctx.builder.CreateAlignedLoad(
						zend_jit_GEP(
							llvm_ctx,
							func_addr,
							offsetof(zend_function, op_array.T),
							PointerType::getUnqual(Type::getInt32Ty(llvm_ctx.context))), 4));
		}
		Value *dst = llvm_ctx.builder.CreateInBoundsGEP(
			src,
			llvm_ctx.builder.CreateSub(
				used_slots,
				first_extra_arg_val));
		//JIT: if (EXPECTED(src != dst)) {
		PHI_DCL2(src, 2);
		PHI_ADD2(src, src);
		PHI_DCL2(dst, 2);
		PHI_ADD2(dst, dst);
		BasicBlock *bb_loop = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		zend_jit_expected_br(llvm_ctx,
			llvm_ctx.builder.CreateICmpNE(
				src,
				dst),
			bb_loop,
			bb_end);		
		//JIT: do {
		llvm_ctx.builder.SetInsertPoint(bb_loop);
		PHI_SET2(src, src, llvm_ctx.zval_ptr_type);
		PHI_SET2(dst, dst, llvm_ctx.zval_ptr_type);
		//JIT: ZVAL_COPY_VALUE(dst, src);
		zend_jit_copy_value(llvm_ctx, 
			dst,
			0,
			-1,
			MAY_BE_ANY,
			src,
			NULL, 
			IS_VAR,
			NULL,
			-1,
			MAY_BE_ANY);
		//JIT: ZVAL_UNDEF(src);
		zend_jit_save_zval_type_info(llvm_ctx, src, -1, MAY_BE_ANY, llvm_ctx.builder.getInt32(IS_UNDEF));
		//JIT: src--;
		src = zend_jit_GEP(
			llvm_ctx,
			src,
			-sizeof(zval),
			llvm_ctx.zval_ptr_type);
		PHI_ADD2(src, src);
		//JIT: dst--;
		dst = zend_jit_GEP(
			llvm_ctx,
			dst,
			-sizeof(zval),
			llvm_ctx.zval_ptr_type);
		PHI_ADD2(dst, dst);
		//JIT: } while (src != end);
		zend_jit_expected_br(llvm_ctx,
			llvm_ctx.builder.CreateICmpNE(
				src,
				end),
			bb_loop,
			bb_end);
		llvm_ctx.builder.SetInsertPoint(bb_no_extra);
		if (func) {
			if (EXPECTED((func->op_array.fn_flags & ZEND_ACC_HAS_TYPE_HINTS) == 0)) {
				/* Skip useless ZEND_RECV and ZEND_RECV_INIT opcodes */
				//JIT: EX(opline) = opline + num_args;
				llvm_ctx.builder.CreateAlignedStore(
					llvm_ctx.builder.CreateAdd(
						opline,
						LLVM_GET_LONG(num_args * sizeof(zend_op))),
					zend_jit_GEP(
						llvm_ctx,
						execute_data,
						offsetof(zend_execute_data, opline),
						PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4);
	        } else {
				//JIT: EX(opline) = opline;
				llvm_ctx.builder.CreateAlignedStore(
					opline,
					zend_jit_GEP(
						llvm_ctx,
						execute_data,
						offsetof(zend_execute_data, opline),
						PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4);
			}
		} else {
			//JIT: if (EXPECTED((op_array->fn_flags & ZEND_ACC_HAS_TYPE_HINTS) == 0)) {
			//JIT: EX(opline) += num_args;
			llvm_ctx.builder.CreateAlignedStore(
				llvm_ctx.builder.CreateAdd(
					opline,
					llvm_ctx.builder.CreateMul(
						num_args_val,
						llvm_ctx.builder.CreateZExtOrBitCast(
							llvm_ctx.builder.CreateICmpEQ(
								llvm_ctx.builder.CreateAnd(
									llvm_ctx.builder.CreateAlignedLoad(
										zend_jit_GEP(
											llvm_ctx,
											func_addr,
											offsetof(zend_function, op_array.fn_flags),
											PointerType::getUnqual(Type::getInt32Ty(llvm_ctx.context))), 4),
									llvm_ctx.builder.getInt32(ZEND_ACC_HAS_TYPE_HINTS)),
								llvm_ctx.builder.getInt32(0)),
							Type::getInt32Ty(llvm_ctx.context)))),
				zend_jit_GEP(
					llvm_ctx,
					execute_data,
					offsetof(zend_execute_data, opline),
					PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4);
        }
		llvm_ctx.builder.CreateBr(bb_end);
		llvm_ctx.builder.SetInsertPoint(bb_end);
	}
#endif

	/* Initialize CV variables (skip arguments) */
	if (func && num_args != -1) {
		uint32_t i;
		for (i = num_args; i < func->op_array.last_var; i++) {
			//JIT: zval *var = EX_VAR_NUM(i);
			Value *var = zend_jit_GEP(
				llvm_ctx,
				execute_data,
				(ZEND_CALL_FRAME_SLOT + i) * sizeof(zval),
				llvm_ctx.zval_ptr_type);
			//JIT: ZVAL_UNDEF(var);
			zend_jit_save_zval_type_info(llvm_ctx, var, -1, MAY_BE_ANY, llvm_ctx.builder.getInt32(IS_UNDEF));
		}
	} else {
//???..check
		Value *num_args_val = NULL;
		Value *last_var_val = NULL;
		if (num_args != -1) {
			num_args_val = llvm_ctx.builder.getInt32(num_args);
		} else {
			num_args_val = 
				llvm_ctx.builder.CreateAlignedLoad(
					zend_jit_GEP(
						llvm_ctx,
						execute_data,
						offsetof(zend_execute_data, This.u2.num_args),
						PointerType::getUnqual(Type::getInt32Ty(llvm_ctx.context))), 4);
		}
		if (func) {
			last_var_val = llvm_ctx.builder.getInt32(func->op_array.last_var);
		} else {
			last_var_val = 
				llvm_ctx.builder.CreateAlignedLoad(
					zend_jit_GEP(
						llvm_ctx,
						func_addr,
						offsetof(zend_function, op_array.last_var),
						PointerType::getUnqual(Type::getInt32Ty(llvm_ctx.context))), 4);
		}
		//JIT: if (EXPECTED(num_args < op_array->last_var)) {
		BasicBlock *bb_init = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_end = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_loop = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		zend_jit_expected_br(llvm_ctx,
			llvm_ctx.builder.CreateICmpULT(
				num_args_val,
				last_var_val),
			bb_init,
			bb_end);
		llvm_ctx.builder.SetInsertPoint(bb_init);
		//JIT: zval *var = EX_VAR_NUM(num_args);
		Value *base = zend_jit_GEP(
			llvm_ctx,
			execute_data,
			ZEND_CALL_FRAME_SLOT * sizeof(zval),
			llvm_ctx.zval_ptr_type);
		Value *var = llvm_ctx.builder.CreateInBoundsGEP(
			base,
			num_args_val);
		//JIT: zval *end = EX_VAR_NUM(op_array->last_var);
		Value *end = llvm_ctx.builder.CreateInBoundsGEP(
			base,
			last_var_val);
		//JIT: do {
		PHI_DCL2(var, 2);
		PHI_ADD2(var, var);
		llvm_ctx.builder.CreateBr(bb_loop);
		llvm_ctx.builder.SetInsertPoint(bb_loop);
		PHI_SET2(var, var, llvm_ctx.zval_ptr_type);
		//JIT: ZVAL_UNDEF(var);
		zend_jit_save_zval_type_info(llvm_ctx, var, -1, MAY_BE_ANY, llvm_ctx.builder.getInt32(IS_UNDEF));
		//JIT: var++;
		var = zend_jit_GEP(
			llvm_ctx,
			var,
			sizeof(zval),
			llvm_ctx.zval_ptr_type);
		PHI_ADD2(var, var);
		//JIT: } while (var != end);
		zend_jit_expected_br(llvm_ctx,
			llvm_ctx.builder.CreateICmpNE(
				var,
				end),
			bb_loop,
			bb_end);
		llvm_ctx.builder.SetInsertPoint(bb_end);
	}

//???..check
	if (!func || func->op_array.this_var != -1) {
		BasicBlock *bb_end = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_follow;
		Value *this_var = NULL;

		if (!func) {
			this_var = llvm_ctx.builder.CreateAlignedLoad(
					zend_jit_GEP(
						llvm_ctx,
						func_addr,
						offsetof(zend_function, op_array.this_var),
						PointerType::getUnqual(Type::getInt32Ty(llvm_ctx.context))), 4);
			//JIT: if (op_array->this_var != -1) {
			bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			zend_jit_expected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpNE(
					this_var,
					llvm_ctx.builder.getInt32(-1)),
				bb_follow,
				bb_end);
			llvm_ctx.builder.SetInsertPoint(bb_follow);
		} else {
			this_var = llvm_ctx.builder.getInt32(func->op_array.this_var);
		}
		//JIT: if (EX(object)) {
		bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		zend_jit_expected_br(llvm_ctx,
			llvm_ctx.builder.CreateIsNotNull(object),
			bb_follow,
			bb_end);
		llvm_ctx.builder.SetInsertPoint(bb_follow);
		//JIT: ZVAL_OBJ(EX_VAR(op_array->this_var), EX(object));
		Value *var =
			llvm_ctx.builder.CreateInBoundsGEP(
				zend_jit_GEP(
					llvm_ctx,
					execute_data,
					ZEND_CALL_FRAME_SLOT * sizeof(zval),
					llvm_ctx.zval_ptr_type),
				this_var);
		zend_jit_save_zval_obj(llvm_ctx, var, -1, MAY_BE_ANY, object);
		zend_jit_save_zval_type_info(llvm_ctx, var, -1, MAY_BE_ANY, llvm_ctx.builder.getInt32(IS_OBJECT_EX));
		//JIT: GC_REFCOUNT(EX(object))++;
		zend_jit_addref(llvm_ctx, object);
		llvm_ctx.builder.CreateBr(bb_end);
		llvm_ctx.builder.SetInsertPoint(bb_end);
	}

	//JIT: EX_LOAD_RUN_TIME_CACHE(op_array);
#if ZEND_EX_USE_RUN_TIME_CACHE
	if (func && !func->op_array.cache_size) {
	    Value *cache = llvm_ctx.builder.CreateAlignedStore(
    		LLVM_GET_LONG(0),
			zend_jit_GEP(
				llvm_ctx,
				execute_data,
				offsetof(zend_execute_data, run_time_cache),
				PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4);
    } else {
    	Value *cache = NULL;
		// We don't have to reinitialize run_time_cache for recursive function calls
		if (llvm_ctx.op_array == &func->op_array) {
			cache = llvm_ctx.builder.CreateAlignedLoad(
				zend_jit_GEP(
					llvm_ctx,
					llvm_ctx._execute_data,
					offsetof(zend_execute_data, run_time_cache),
					PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4);
		} else {
			cache = llvm_ctx.builder.CreateAlignedLoad(
				zend_jit_GEP(
					llvm_ctx,
					func_addr,
					offsetof(zend_function, op_array.run_time_cache),
					PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4);
			//JIT: if (!op_array->run_time_cache && op_array->last_cache_slot) {
			BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			BasicBlock *bb_common = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			PHI_DCL(cache, 3);

			PHI_ADD(cache, cache);
			zend_jit_unexpected_br(llvm_ctx,
				llvm_ctx.builder.CreateIsNull(cache),
				bb_follow,
				bb_common);
			llvm_ctx.builder.SetInsertPoint(bb_follow);

			if (!func) {
				BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				PHI_ADD(cache, cache);
				zend_jit_expected_br(llvm_ctx,
					llvm_ctx.builder.CreateICmpNE(
						llvm_ctx.builder.CreateAlignedLoad(
							zend_jit_GEP(
								llvm_ctx,
								func_addr,
								offsetof(zend_function, op_array.cache_size),
								PointerType::getUnqual(Type::getInt32Ty(llvm_ctx.context))), 4),
						llvm_ctx.builder.getInt32(0)),
					bb_follow,
					bb_common);
				llvm_ctx.builder.SetInsertPoint(bb_follow);			
			}			
			//JIT: op_array->run_time_cache = zend_arena_calloc(&CG(arena), op_array->last_cache_slot, sizeof(void*));
#if 1
			cache =
				llvm_ctx.builder.CreatePtrToInt(
					zend_jit_arena_calloc(llvm_ctx,
						llvm_ctx._CG_arena,
						(func ? 
							(Value*)llvm_ctx.builder.getInt32(func->op_array.cache_size) :
							(Value*)llvm_ctx.builder.CreateAlignedLoad(
								zend_jit_GEP(
									llvm_ctx,
									func_addr,
									offsetof(zend_function, op_array.cache_size),
									PointerType::getUnqual(Type::getInt32Ty(llvm_ctx.context))), 4)),
						1,
						lineno),
					LLVM_GET_LONG_TY(llvm_ctx.context));
#else
			Value *cache_size =	(func ? 
							(Value*)llvm_ctx.builder.getInt32(func->op_array.cache_size) :
							(Value*)llvm_ctx.builder.CreateAlignedLoad(
								zend_jit_GEP(
									llvm_ctx,
									func_addr,
									offsetof(zend_function, op_array.cache_size),
									PointerType::getUnqual(Type::getInt32Ty(llvm_ctx.context))), 4));
			Value *cache_addr =
				zend_jit_arena_alloc(llvm_ctx,
					llvm_ctx._CG_arena,
					cache_size,
					lineno);
			cache =
				llvm_ctx.builder.CreatePtrToInt(
					cache_addr,
					LLVM_GET_LONG_TY(llvm_ctx.context));
			//JIT: memset(cache, 0, size);
			llvm_ctx.builder.CreateMemSet(
				cache_addr,
				llvm_ctx.builder.getInt8(0),
				cache_size,
				4);
#endif
		    llvm_ctx.builder.CreateAlignedStore(
    			cache,
				zend_jit_GEP(
					llvm_ctx,
					func_addr,
					offsetof(zend_function, op_array.run_time_cache),
					PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4);
			PHI_ADD(cache, cache);
			llvm_ctx.builder.CreateBr(bb_common);
			//JIT: EX(run_time_cache) = op_array->run_time_cache;
			llvm_ctx.builder.SetInsertPoint(bb_common);
			PHI_SET(cache, cache, LLVM_GET_LONG_TY(llvm_ctx.context));
		}
	    llvm_ctx.builder.CreateAlignedStore(
    		cache,
			zend_jit_GEP(
				llvm_ctx,
				execute_data,
				offsetof(zend_execute_data, run_time_cache),
				PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4);
	}
#endif

	//JIT: EX_LOAD_LITERALS(op_array);
#if ZEND_EX_USE_LITERALS
	if (func && !func->op_array.last_literal) {
		llvm_ctx.builder.CreateAlignedStore(
    		LLVM_GET_LONG(0),
			zend_jit_GEP(
				llvm_ctx,
				execute_data,
				offsetof(zend_execute_data, literals),
				PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4);
    } else {
		Value *literals = llvm_ctx.builder.CreateAlignedLoad(
				zend_jit_GEP(
					llvm_ctx,
					func_addr,
					offsetof(zend_function, op_array.literals),
					PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4);
		llvm_ctx.builder.CreateAlignedStore(
    		literals,
			zend_jit_GEP(
				llvm_ctx,
				execute_data,
				offsetof(zend_execute_data, literals),
				PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4);
	}
#endif

	//JIT: EG(current_execute_data) = execute_data;
	llvm_ctx.builder.CreateAlignedStore(
		execute_data,
		llvm_ctx._EG_current_execute_data, 4);
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_set_retref_flag */
static int zend_jit_set_retref_flag(zend_llvm_ctx    &llvm_ctx,
                                    Value            *ret,
                                    zend_function    *func,
                                    Value            *func_addr)
{
	if (func) {
		if ((func->common.fn_flags & ZEND_ACC_RETURN_REFERENCE) != 0) {
			llvm_ctx.builder.CreateAlignedStore(
				llvm_ctx.builder.getInt32(IS_VAR_RET_REF),
				zend_jit_GEP(
					llvm_ctx,
					ret,
					offsetof(zval, u2.var_flags),
					PointerType::getUnqual(Type::getInt32Ty(llvm_ctx.context))), 4);
		} else {
			llvm_ctx.builder.CreateAlignedStore(
				llvm_ctx.builder.getInt32(0),
				zend_jit_GEP(
					llvm_ctx,
					ret,
					offsetof(zval, u2.var_flags),
					PointerType::getUnqual(Type::getInt32Ty(llvm_ctx.context))), 4);
		}
	} else {
		BasicBlock *bb_ref = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_not_ref = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		BasicBlock *bb_common = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		zend_jit_unexpected_br(llvm_ctx,
			llvm_ctx.builder.CreateICmpNE(
				llvm_ctx.builder.CreateAnd(
					llvm_ctx.builder.CreateAlignedLoad(
						zend_jit_GEP(
							llvm_ctx,
							func_addr,
							offsetof(zend_function, common.fn_flags),
							PointerType::getUnqual(Type::getInt32Ty(llvm_ctx.context))), 4),
					llvm_ctx.builder.getInt32(ZEND_ACC_RETURN_REFERENCE)),
				llvm_ctx.builder.getInt32(0)),					
			bb_ref,
			bb_not_ref);
		llvm_ctx.builder.SetInsertPoint(bb_ref);
		llvm_ctx.builder.CreateAlignedStore(
			llvm_ctx.builder.getInt32(IS_VAR_RET_REF),
			zend_jit_GEP(
				llvm_ctx,
				ret,
				offsetof(zval, u2.var_flags),
				PointerType::getUnqual(Type::getInt32Ty(llvm_ctx.context))), 4);
		llvm_ctx.builder.CreateBr(bb_common);
		llvm_ctx.builder.SetInsertPoint(bb_not_ref);
		llvm_ctx.builder.CreateAlignedStore(
			llvm_ctx.builder.getInt32(0),
			zend_jit_GEP(
				llvm_ctx,
				ret,
				offsetof(zval, u2.var_flags),
				PointerType::getUnqual(Type::getInt32Ty(llvm_ctx.context))), 4);
		llvm_ctx.builder.CreateBr(bb_common);
		llvm_ctx.builder.SetInsertPoint(bb_common);
	}
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_check_internal_type_hint */
static int zend_jit_check_internal_type_hint(zend_llvm_ctx    &llvm_ctx,
                                             Value            *func,
                                             Value            *arg_num,
                                             Value            *arg)
{
	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)zend_check_internal_arg_type,
			ZEND_JIT_SYM("zend_check_internal_arg_type"),
			ZEND_JIT_HELPER_FAST_CALL,
			Type::getVoidTy(llvm_ctx.context),
			PointerType::getUnqual(llvm_ctx.zend_function_type),
			Type::getInt32Ty(llvm_ctx.context),
			llvm_ctx.zval_ptr_type,
			NULL,
			NULL);

	CallInst *call = llvm_ctx.builder.CreateCall3(_helper,
		func, arg_num, arg);
	call->setCallingConv(CallingConv::X86_FastCall);
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_check_type_hint */
static Value* zend_jit_check_type_hint(zend_llvm_ctx    &llvm_ctx,
                                    Value            *func,
                                    Value            *arg_num,
                                    Value            *arg,
                                    Value            *default_value,
                                    Value            *cache_slot)
{
	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)zend_check_arg_type,
			ZEND_JIT_SYM("zend_check_arg_type"),
			ZEND_JIT_HELPER_FAST_CALL,
			Type::getInt32Ty(llvm_ctx.context),
			PointerType::getUnqual(llvm_ctx.zend_function_type),
			Type::getInt32Ty(llvm_ctx.context),
			llvm_ctx.zval_ptr_type,
			llvm_ctx.zval_ptr_type,
			PointerType::getUnqual(PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))));

	CallInst *call = llvm_ctx.builder.CreateCall5(_helper,
		func, arg_num, arg, default_value, cache_slot);
	call->setCallingConv(CallingConv::X86_FastCall);
	return call;
}
/* }}} */

/* {{{ static int zend_jit_check_missing_arg */
static void zend_jit_check_missing_arg(zend_llvm_ctx    &llvm_ctx,
                                      uint32_t          arg_num,
                                      Value            *cache_slot)
{
	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)zend_check_missing_arg,
			ZEND_JIT_SYM("zend_check_missing_arg"),
			ZEND_JIT_HELPER_FAST_CALL,
			Type::getVoidTy(llvm_ctx.context),
			PointerType::getUnqual(llvm_ctx.zend_execute_data_type),
			Type::getInt32Ty(llvm_ctx.context),
			PointerType::getUnqual(PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))),
			NULL,
			NULL);

	CallInst *call = llvm_ctx.builder.CreateCall3(_helper,
		llvm_ctx._execute_data,
		llvm_ctx.builder.getInt32(arg_num),
		cache_slot);
	call->setCallingConv(CallingConv::X86_FastCall);
}
/* }}} */

/* {{{ static int zend_jit_update_constant */
static Value *zend_jit_update_constant(zend_llvm_ctx    &llvm_ctx,
                                       Value            *val,
                                       uint32_t          inline_change)
{
	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)zval_update_constant_ex,
			ZEND_JIT_SYM("zval_update_constant_ex"),
			0,
			Type::getInt32Ty(llvm_ctx.context),
			llvm_ctx.zval_ptr_type,
			Type::getInt8Ty(llvm_ctx.context),
			PointerType::getUnqual(llvm_ctx.zend_class_entry_type),
			NULL,
			NULL);

	CallInst *call = llvm_ctx.builder.CreateCall3(_helper,
		val,
		llvm_ctx.builder.getInt8(inline_change),
		llvm_ctx.builder.CreateIntToPtr(
			LLVM_GET_LONG(0),
			PointerType::getUnqual(llvm_ctx.zend_class_entry_type)));
	return call;
}
/* }}} */

/* {{{ static int zend_jit_check_type_hints */
static int zend_jit_check_type_hints(zend_llvm_ctx    &llvm_ctx,
                                     zend_function    *func,
                                     Value            *func_addr,
                                     uint32_t          num_args,
                                     Value            *call)
{
	int ret = 1;

	if (!func || (func->common.fn_flags & ZEND_ACC_HAS_TYPE_HINTS)) {
		if (func && num_args != -1) {
			uint32_t i;
			zend_arg_info *cur_arg_info;
			for (i = 0; i < num_args; i++) {
				if (i < func->common.num_args) {
					cur_arg_info = &func->common.arg_info[i];
				} else if (func->common.fn_flags & ZEND_ACC_VARIADIC) {
					cur_arg_info = &func->common.arg_info[func->common.num_args];
				} else {
					break;
				}
				if (cur_arg_info->class_name || cur_arg_info->type_hint) {
					ret = 2;
					zend_jit_check_internal_type_hint(
						llvm_ctx,
						func_addr,
						llvm_ctx.builder.getInt32(i + 1),
						zend_jit_GEP(
							llvm_ctx,
							call,
							(ZEND_CALL_FRAME_SLOT + i) * sizeof(zval),
							llvm_ctx.zval_ptr_type));
				}
			}
		} else {
			//TODO: not implemented
			ASSERT_NOT_REACHED();		
    	}
	}
	return ret;
}
/* }}} */

/* {{{ static int zend_jit_find_num_args */
static uint32_t zend_jit_find_num_args(zend_op_array *op_array,
                                       zend_op       *opline)
{
	int level = 0;

	do {	
		opline--;
		switch (opline->opcode) {
			case ZEND_DO_FCALL:
			case ZEND_DO_ICALL:
			case ZEND_DO_UCALL:
			case ZEND_DO_FCALL_BY_NAME:
				level++;
				break;
			case ZEND_INIT_FCALL:
			case ZEND_INIT_FCALL_BY_NAME:
			case ZEND_INIT_NS_FCALL_BY_NAME:
			case ZEND_INIT_DYNAMIC_CALL:
			case ZEND_INIT_METHOD_CALL:
			case ZEND_INIT_STATIC_METHOD_CALL:
			case ZEND_INIT_USER_CALL:
			case ZEND_NEW:
				if (level == 0) {
					return opline->extended_value;
				}
				level--;
				break;
			case ZEND_SEND_UNPACK:
			case ZEND_SEND_ARRAY:
			case ZEND_SEND_USER: /* zend_call_user_func() ??? */
				if (level == 0) {
					return -1;
				}
				break;
			default:
				break;				
		}			
	} while (opline != op_array->opcodes);
	return -1;
}
/* }}} */

/* {{{ static void collect_depended_phi_vars */
static void collect_depended_phi_vars(zend_bitset         worklist,
                                      zend_jit_func_info *info,
                                      int                 var)
{
	zend_bitset_incl(worklist, var);

	if (info->ssa.var[var].phi_use_chain) {		
		zend_jit_ssa_phi *p = info->ssa.var[var].phi_use_chain;
		do {
			if (!zend_bitset_in(worklist, p->ssa_var)) {
				collect_depended_phi_vars(worklist, info, p->ssa_var);
			}
			p = next_use_phi(info, var, p);
		} while (p);
	}
}
/* }}} */

/* {{{ static int zend_jit_may_be_used_as_returned_reference */
static int zend_jit_may_be_used_as_returned_reference(zend_op_array *op_array,
                                                      int            var)
{
	zend_jit_func_info *info = JIT_DATA(op_array);
	zend_bitset worklist;
	int worklist_len;
	int j, i;

	if (!info->ssa.var) {
		return 1;
	}

	worklist_len = zend_bitset_len(info->ssa.vars);
	worklist = (zend_bitset)alloca(sizeof(zend_ulong) * worklist_len);
	memset(worklist, 0, sizeof(zend_ulong) * worklist_len);
	collect_depended_phi_vars(worklist, info, var);

	while (!zend_bitset_empty(worklist, worklist_len)) {
		i = zend_bitset_first(worklist, worklist_len);
		zend_bitset_excl(worklist, i);
		j = info->ssa.var[i].use_chain;
		while (j >= 0) {
			zend_op *opline = op_array->opcodes + j;
			if (opline->extended_value == ZEND_RETURNS_FUNCTION) {
				switch (opline->opcode) {
					case ZEND_ASSIGN_REF:
						if (info->ssa.op[j].op2_use == i) {
							return 1;
						}
						break;
					case ZEND_RETURN_BY_REF:
					case ZEND_SEND_VAR_NO_REF:
					case ZEND_YIELD:
					case ZEND_YIELD_FROM:
						if (info->ssa.op[j].op1_use == i) {
							return 1;
						}
						break;
				}
		    }
			j = next_use(info->ssa.op, i, j);
		}
	}

	return 0;
}
/* }}} */

/* {{{ static int zend_jit_do_fcall */
static int zend_jit_do_fcall(zend_llvm_ctx    &llvm_ctx,
                             zend_jit_context *ctx,
                             zend_op_array    *op_array,
                             zend_op          *opline)
{
	zend_jit_func_info *info = JIT_DATA(op_array);
	zend_jit_call_info *call_info = info->callee_info;
	zend_jit_func_info *func_info = NULL;
	zend_function *func = NULL;
	Value *func_addr = NULL;
	BasicBlock *bb_fcall_end_scope = NULL;
	BasicBlock *bb_fcall_end = NULL;

	while (call_info && call_info->caller_call_opline != opline) {
		call_info = call_info->next_callee;
	}

	if (call_info) {
		if (call_info->callee_func) {
			func = call_info->callee_func;
			if (func && func->type == ZEND_USER_FUNCTION) {
				func_info = JIT_DATA(&func->op_array);
			}
		}
	}
	uint32_t num_args = zend_jit_find_num_args(op_array, opline);

	//TODO: generate code only for known functions with known number of passed arguments
	if (!func || num_args == -1) {
		return zend_jit_handler(llvm_ctx, opline);
	}


	//JIT: zend_execute_data *call = EX(call);
	Value *call = llvm_ctx.builder.CreateAlignedLoad(
			zend_jit_GEP(
				llvm_ctx,
				llvm_ctx._execute_data,
				offsetof(zend_execute_data, call),
				PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.zend_execute_data_type))), 4);
	//JIT: zend_object *object = Z_OBJ(call->This);
	Value *object = NULL;
	if (!call_info || call_info->caller_init_opline->opcode != ZEND_INIT_FCALL) {
		//TODO: not only ZEND_DO_FCALL
		object = llvm_ctx.builder.CreateAlignedLoad(
			zend_jit_GEP(
				llvm_ctx,
				call,
				offsetof(zend_execute_data, This.value.obj),
				PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.zend_object_type))), 4);
	}

	if (func && func->type == ZEND_INTERNAL_FUNCTION) {
		func_addr = llvm_ctx.builder.CreateIntToPtr(
				LLVM_GET_LONG((zend_uintptr_t)func),
				PointerType::getUnqual(llvm_ctx.zend_function_type));
	} else {
		//JIT: zend_function *fbc = call->func;
		func_addr = llvm_ctx.builder.CreateAlignedLoad(
				zend_jit_GEP(
					llvm_ctx,
					call,
					offsetof(zend_execute_data, func),
					PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.zend_function_type))), 4);
	}

	//JIT: SAVE_OPLINE();
	if (!llvm_ctx.valid_opline) {
		JIT_CHECK(zend_jit_store_opline(llvm_ctx, opline, true));
	}
	//JIT: EX(call) = call->prev_execute_data;
	Value *prev = NULL;
	if (llvm_ctx.call_level == 1) {
		prev = llvm_ctx.builder.CreateIntToPtr(
			LLVM_GET_LONG(0),
			PointerType::getUnqual(llvm_ctx.zend_execute_data_type));
	} else {
		prev = llvm_ctx.builder.CreateAlignedLoad(
			zend_jit_GEP(
				llvm_ctx,
				call,
				offsetof(zend_execute_data, prev_execute_data),
				PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.zend_execute_data_type))), 4);
	}
	llvm_ctx.builder.CreateAlignedStore(
	    prev,
		zend_jit_GEP(
			llvm_ctx,
			llvm_ctx._execute_data,
			offsetof(zend_execute_data, call),
			PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.zend_execute_data_type))), 4);

	if (func) {
		if ((func->common.fn_flags & ZEND_ACC_ABSTRACT) != 0) {
			zend_jit_error_noreturn(llvm_ctx, opline, E_ERROR, 
				"Cannot call abstract method %s::%s()",
				LLVM_GET_CONST_STRING(func->common.scope->name->val),
				LLVM_GET_CONST_STRING(func->common.function_name->val));
			llvm_ctx.valid_opline = 0;
			return 1;

		}
		if ((func->common.fn_flags & ZEND_ACC_DEPRECATED) != 0) {
			zend_jit_error(llvm_ctx, opline, E_DEPRECATED,
				"Function %s%s%s() is deprecated",
				func->common.scope ? LLVM_GET_CONST_STRING(func->common.scope->name->val) : LLVM_GET_CONST_STRING(""),
				func->common.scope ? LLVM_GET_CONST_STRING("::") : LLVM_GET_CONST_STRING(""),
				LLVM_GET_CONST_STRING(func->common.function_name->val));
			zend_jit_check_exception(llvm_ctx, opline);
		}
	} else {
		//JIT: if (UNEXPECTED((fbc->common.fn_flags & (ZEND_ACC_ABSTRACT|ZEND_ACC_DEPRECATED)) != 0)) {
		
		//JIT: if (UNEXPECTED((fbc->common.fn_flags & ZEND_ACC_ABSTRACT) != 0)) {
		//JIT: zend_error_noreturn(E_ERROR, "Cannot call abstract method %s::%s()", fbc->common.scope->name->val, fbc->common.function_name->val);

		//JIT: if (UNEXPECTED((fbc->common.fn_flags & ZEND_ACC_DEPRECATED) != 0)) {
		//JIT: zend_error(E_DEPRECATED, "Function %s%s%s() is deprecated",
		//       fbc->common.scope ? fbc->common.scope->name->val : "",
		//       fbc->common.scope ? "::" : "",
		//       fbc->common.function_name->val);
		//JIT: if (UNEXPECTED(EG(exception) != NULL)) HANDLE_EXCEPTION();
		
		//???... error messages
	}

	Value *func_type = NULL;
	
	if (!func || func->type == ZEND_INTERNAL_FUNCTION) {
		BasicBlock *bb_follow = NULL;
		if (!func) {
			//JIT: if (UNEXPECTED(fbc->type == ZEND_INTERNAL_FUNCTION)) {
			func_type = llvm_ctx.builder.CreateAlignedLoad(
				zend_jit_GEP(
					llvm_ctx,
					func_addr,
					offsetof(zend_function, type),
					PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context))), 4);
			BasicBlock *bb_internal  = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

			zend_jit_unexpected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(
					func_type,
					llvm_ctx.builder.getInt8(ZEND_INTERNAL_FUNCTION)),
				bb_internal,
				bb_follow);
			llvm_ctx.builder.SetInsertPoint(bb_internal);
		}

		do {
			BasicBlock *bb_common = NULL;
			BasicBlock *bb_func = NULL;

			if (!func || func->common.scope) {
				if (!func) {
					BasicBlock *bb_method = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					bb_func = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					bb_common = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					zend_jit_unexpected_br(llvm_ctx,
						llvm_ctx.builder.CreateIsNotNull(
							llvm_ctx.builder.CreateAlignedLoad(
								zend_jit_GEP(
									llvm_ctx,
									func_addr,
									offsetof(zend_function, common.scope),
									PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.zend_class_entry_type))), 4)),
						bb_method,
						bb_func);
					llvm_ctx.builder.SetInsertPoint(bb_method);
				}
				//JIT: EG(scope) = object ? NULL : func->common.scope;
				if (!bb_common) {
					bb_common = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				}
				if (object) {
					BasicBlock *bb_null = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					BasicBlock *bb_not_null = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					zend_jit_expected_br(llvm_ctx,
						llvm_ctx.builder.CreateIsNotNull(object),
						bb_not_null,
						bb_null);
					llvm_ctx.builder.SetInsertPoint(bb_not_null);
					llvm_ctx.builder.CreateAlignedStore(
						llvm_ctx.builder.CreateIntToPtr(
								LLVM_GET_LONG(0),
								PointerType::getUnqual(llvm_ctx.zend_class_entry_type)),
						llvm_ctx._EG_scope, 4);
					llvm_ctx.builder.CreateBr(bb_common);
					llvm_ctx.builder.SetInsertPoint(bb_null);
				}
				if (func) {
					llvm_ctx.builder.CreateAlignedStore(
						llvm_ctx.builder.CreateIntToPtr(
							LLVM_GET_LONG((zend_uintptr_t)func->common.scope),
							PointerType::getUnqual(llvm_ctx.zend_class_entry_type)),
						llvm_ctx._EG_scope, 4);
				} else {
					llvm_ctx.builder.CreateAlignedStore(
						llvm_ctx.builder.CreateAlignedLoad(
							zend_jit_GEP(
								llvm_ctx,
								func_addr,
								offsetof(zend_function, common.scope),
								PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.zend_class_entry_type))), 4),
						llvm_ctx._EG_scope, 4); 
				}
				llvm_ctx.builder.CreateBr(bb_common);
				if (bb_func) {
					llvm_ctx.builder.SetInsertPoint(bb_func);
				}
			}

			if (!func || !func->common.scope) {
				//JIT: call->called_scope = EX(called_scope);
				llvm_ctx.builder.CreateAlignedStore(
					llvm_ctx.builder.CreateAlignedLoad(
						zend_jit_GEP(
							llvm_ctx,
							llvm_ctx._execute_data,
							offsetof(zend_execute_data, called_scope),
							PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.zend_class_entry_type))), 4),
					zend_jit_GEP(
						llvm_ctx,
						call,
						offsetof(zend_execute_data, called_scope),
						PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.zend_class_entry_type))), 4);
				if (bb_common) {
					llvm_ctx.builder.CreateBr(bb_common);
				}
			}		
			if (bb_common) {
				llvm_ctx.builder.SetInsertPoint(bb_common);
			}
	    } while (0);
		
		//JIT: call->prev_execute_data = execute_data;
		llvm_ctx.builder.CreateAlignedStore(
			llvm_ctx._execute_data,
			zend_jit_GEP(
				llvm_ctx,
				call,
				offsetof(zend_execute_data, prev_execute_data),
				PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.zend_execute_data_type))), 4);
		//JIT: EG(current_execute_data) = call;
		llvm_ctx.builder.CreateAlignedStore(
			call,
			llvm_ctx._EG_current_execute_data, 4);

		if (zend_jit_check_type_hints(llvm_ctx, func, func_addr, num_args, call) == 2) {
			//JIT: if (UNEXPECTED(EG(exception) != NULL)) {
			BasicBlock *bb_skip = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
    		BasicBlock *bb_exception = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			zend_jit_unexpected_br(llvm_ctx,
				llvm_ctx.builder.CreateIsNotNull(
					llvm_ctx.builder.CreateAlignedLoad(llvm_ctx._EG_exception, 4, 1)),
				bb_exception,
				bb_skip);
			llvm_ctx.builder.SetInsertPoint(bb_exception);
			//JIT: EG(current_execute_data) = call->prev_execute_data;		
			llvm_ctx.builder.CreateAlignedStore(
				llvm_ctx.builder.CreateAlignedLoad(
					zend_jit_GEP(
						llvm_ctx,
						call,
						offsetof(zend_execute_data, prev_execute_data),
						PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.zend_execute_data_type))), 4),
				llvm_ctx._EG_current_execute_data, 4);
			//JIT: zend_vm_stack_free_args(call TSRMLS_CC);
			zend_jit_vm_stack_free_args(llvm_ctx,
				num_args,
				call,
				opline);
			//JIT: zend_vm_stack_free_call_frame(call TSRMLS_CC);
			zend_jit_vm_stack_free_call_frame(llvm_ctx, call, opline->lineno);
			if (RETURN_VALUE_USED(opline)) {
				//JIT: ZVAL_UNDEF(EX_VAR(RES_OP()->var));
				Value *ret = zend_jit_load_var(llvm_ctx, RES_OP()->var);
				zend_jit_save_zval_type_info(llvm_ctx, ret, RES_SSA_VAR(), RES_INFO(), llvm_ctx.builder.getInt32(IS_UNDEF));
			}
			//JIT: if (UNEXPECTED(should_change_scope)) {
			//JIT: ZEND_VM_C_GOTO(fcall_end_change_scope);
			//JIT: } else {
			//JIT: ZEND_VM_C_GOTO(fcall_end);
			if (func) {
				if (func->common.scope) {
					if (!bb_fcall_end_scope) {
						bb_fcall_end_scope = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					}
					llvm_ctx.builder.CreateBr(bb_fcall_end_scope);
				} else {
					if (!bb_fcall_end) {
						bb_fcall_end = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					}
					llvm_ctx.builder.CreateBr(bb_fcall_end);
				}
			} else {
				if (!bb_fcall_end_scope) {
					bb_fcall_end_scope = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				}
				if (!bb_fcall_end) {
					bb_fcall_end = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				}
				zend_jit_unexpected_br(llvm_ctx,
					llvm_ctx.builder.CreateIsNotNull(
						llvm_ctx.builder.CreateAlignedLoad(
							zend_jit_GEP(
								llvm_ctx,
								func_addr,
								offsetof(zend_function, common.scope),
								PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.zend_class_entry_type))), 4)),
					bb_fcall_end_scope,
					bb_fcall_end);
			}
			llvm_ctx.builder.SetInsertPoint(bb_skip);
		}

		//JIT: ret = EX_VAR(RES_OP()->var);
		Value *ret = zend_jit_load_var(llvm_ctx, RES_OP()->var);
		//JIT: ZVAL_NULL(ret);
		zend_jit_save_zval_type_info(llvm_ctx, ret, RES_SSA_VAR(), RES_INFO(), llvm_ctx.builder.getInt32(IS_NULL));
		if (RETURN_VALUE_USED(opline)) {
			if (zend_jit_may_be_used_as_returned_reference(op_array, RES_SSA_VAR())) {
				//JIT: Z_VAR_FLAGS_P(ret) = (fbc->common.fn_flags & ZEND_ACC_RETURN_REFERENCE) != 0 ? IS_VAR_RET_REF : 0;
				zend_jit_set_retref_flag(llvm_ctx, ret, func, func_addr);
			}
		}

		//JIT: fbc->internal_function.handler(call, ret TSRMLS_CC);
		if (func) {
			Function *_helper = zend_jit_get_helper(
				llvm_ctx,
				(void*)func->internal_function.handler,
				ZEND_JIT_SYM(Twine("PHP__") + func->internal_function.function_name->val),
				0,
				Type::getVoidTy(llvm_ctx.context),
				PointerType::getUnqual(llvm_ctx.zend_execute_data_type),
				llvm_ctx.zval_ptr_type);
			llvm_ctx.builder.CreateCall2(_helper, call, ret);
		} else {
//???..check
			Value *handler =
				llvm_ctx.builder.CreateIntToPtr(
					llvm_ctx.builder.CreateAlignedLoad(
						zend_jit_GEP(
							llvm_ctx,
							func_addr,
							offsetof(zend_function, internal_function.handler),
							PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))), 4),
					PointerType::getUnqual(llvm_ctx.internal_func_type));
			llvm_ctx.builder.CreateCall2(handler, call, ret);
		}

		//JIT: EG(current_execute_data) = call->prev_execute_data;		
		llvm_ctx.builder.CreateAlignedStore(
			llvm_ctx.builder.CreateAlignedLoad(
				zend_jit_GEP(
					llvm_ctx,
					call,
					offsetof(zend_execute_data, prev_execute_data),
					PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.zend_execute_data_type))), 4),
			llvm_ctx._EG_current_execute_data, 4);
		//JIT: zend_vm_stack_free_args(call TSRMLS_CC);
		zend_jit_vm_stack_free_args(llvm_ctx,
			num_args,
			call,
			opline);
		//JIT: zend_vm_stack_free_call_frame(call TSRMLS_CC);
		zend_jit_vm_stack_free_call_frame(llvm_ctx, call, opline->lineno);

		if (!RETURN_VALUE_USED(opline)) {
			//JIT: zval_ptr_dtor(ret);
			Value *ret = zend_jit_load_var(llvm_ctx, RES_OP()->var);
			zend_jit_zval_ptr_dtor_ex(llvm_ctx, ret, NULL, RES_SSA_VAR(), RES_INFO(), opline->lineno, 1); 
		}

		//JIT: if (UNEXPECTED(should_change_scope)) {
		//JIT: ZEND_VM_C_GOTO(fcall_end_change_scope);
		//JIT: } else {
		//JIT: ZEND_VM_C_GOTO(fcall_end);
		if (func) {
			if (func->common.scope) {
				if (!bb_fcall_end_scope) {
					bb_fcall_end_scope = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				}
				llvm_ctx.builder.CreateBr(bb_fcall_end_scope);
			} else {
				if (!bb_fcall_end) {
					bb_fcall_end = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				}
				llvm_ctx.builder.CreateBr(bb_fcall_end);
			}
		} else {
			if (!bb_fcall_end_scope) {
				bb_fcall_end_scope = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			if (!bb_fcall_end) {
				bb_fcall_end = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			zend_jit_unexpected_br(llvm_ctx,
				llvm_ctx.builder.CreateIsNotNull(
					llvm_ctx.builder.CreateAlignedLoad(
						zend_jit_GEP(
							llvm_ctx,
							func_addr,
							offsetof(zend_function, common.scope),
							PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.zend_class_entry_type))), 4)),
				bb_fcall_end_scope,
				bb_fcall_end);
		}

		if (bb_follow) {
			llvm_ctx.builder.SetInsertPoint(bb_follow);
		}
	}
	
	if (!func || func->type == ZEND_USER_FUNCTION) {
		BasicBlock *bb_follow = NULL;
		if (!func) {
			//JIT: if (UNEXPECTED(fbc->type == ZEND_USER_FUNCTION)) {
			BasicBlock *bb_user  = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

			zend_jit_expected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(
					func_type,
					llvm_ctx.builder.getInt8(ZEND_USER_FUNCTION)),
				bb_user,
				bb_follow);
			llvm_ctx.builder.SetInsertPoint(bb_user);
		}

		//JIT: EG(scope) = fbc->common.scope;
		if (func) {
			if (!func ||
			    func->op_array.type != ZEND_USER_FUNCTION ||
			    op_array->type != ZEND_USER_FUNCTION ||
			    !op_array->function_name ||
		    	op_array->scope != func->op_array.scope) {
				llvm_ctx.builder.CreateAlignedStore(
					llvm_ctx.builder.CreateIntToPtr(
						LLVM_GET_LONG((zend_uintptr_t)func->common.scope),
						PointerType::getUnqual(llvm_ctx.zend_class_entry_type)),
					llvm_ctx._EG_scope, 4);
			}
		} else {
		    Value *scope = llvm_ctx.builder.CreateAlignedLoad(
				zend_jit_GEP(
					llvm_ctx,
					func_addr,
					offsetof(zend_function, common.scope),
					PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.zend_class_entry_type))), 4);
			llvm_ctx.builder.CreateAlignedStore(
				scope,
				llvm_ctx._EG_scope, 4);
		}
		//JIT: call->symbol_table = NULL;
		llvm_ctx.builder.CreateAlignedStore(
			llvm_ctx.builder.CreateIntToPtr(
				LLVM_GET_LONG(0),
				PointerType::getUnqual(llvm_ctx.HashTable_type)),
			zend_jit_GEP(
				llvm_ctx,
				call,
				offsetof(zend_execute_data, symbol_table),
				PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.HashTable_type))), 4);

		Value *return_value = NULL;
		if (RETURN_VALUE_USED(opline)) {
			//JIT: return_value = EX_VAR(RES_OP()->var);
			return_value = zend_jit_load_slot(llvm_ctx, RES_OP()->var);
			//JIT: ZVAL_NULL(return_value);
			zend_jit_save_zval_type_info(llvm_ctx, return_value, RES_SSA_VAR(), RES_INFO(), llvm_ctx.builder.getInt32(IS_NULL));
			if (zend_jit_may_be_used_as_returned_reference(op_array, RES_SSA_VAR())) {
				//JIT: Z_VAR_FLAGS_P(return_value) = 0;
				llvm_ctx.builder.CreateAlignedStore(
					llvm_ctx.builder.getInt32(0),
					zend_jit_GEP(
						llvm_ctx,
						return_value,
						offsetof(zval, u2.var_flags),
						PointerType::getUnqual(Type::getInt32Ty(llvm_ctx.context))), 4);
			}
		} else {
			return_value = llvm_ctx.builder.CreateIntToPtr(
				LLVM_GET_LONG(0),
				llvm_ctx.zval_ptr_type);
		}

		if (!func || (func->common.fn_flags & ZEND_ACC_GENERATOR) != 0) {
		    BasicBlock *bb_follow = NULL;
			if (!func) {
				//JIT: if (UNEXPECTED((fbc->common.fn_flags & ZEND_ACC_GENERATOR) != 0)) {
				BasicBlock *bb_generator = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				zend_jit_unexpected_br(llvm_ctx,
					llvm_ctx.builder.CreateICmpNE(
						llvm_ctx.builder.CreateAnd(
							llvm_ctx.builder.CreateAlignedLoad(
								zend_jit_GEP(
									llvm_ctx,
									func_addr,
									offsetof(zend_function, common.fn_flags),
									PointerType::getUnqual(Type::getInt32Ty(llvm_ctx.context))), 4),
							llvm_ctx.builder.getInt32(ZEND_ACC_GENERATOR)),
						llvm_ctx.builder.getInt32(0)),					
					bb_generator,
					bb_follow);
				llvm_ctx.builder.SetInsertPoint(bb_generator);
			}
			if (RETURN_VALUE_USED(opline)) {
				//JIT: zend_generator_create_zval(call, &fbc->op_array, EX_VAR(RES_OP()->var) TSRMLS_CC);
				Function *_helper = zend_jit_get_helper(
					llvm_ctx,
					(void*)zend_generator_create_zval,
					ZEND_JIT_SYM("zend_generator_create_zval"),
					0,
					Type::getVoidTy(llvm_ctx.context),
					PointerType::getUnqual(llvm_ctx.zend_execute_data_type),
					PointerType::getUnqual(llvm_ctx.zend_function_type),
					llvm_ctx.zval_ptr_type,
					NULL,
					NULL);
				llvm_ctx.builder.CreateCall3(_helper, call, func_addr, return_value);
			} else {
				//JIT: zend_vm_stack_free_args(call TSRMLS_CC);
				zend_jit_vm_stack_free_args(llvm_ctx, 
					num_args,
					call,
					opline);
			}
			//JIT: zend_vm_stack_free_call_frame(call TSRMLS_CC);
			zend_jit_vm_stack_free_call_frame(llvm_ctx, 
				call,
				opline->lineno);

			if (!bb_fcall_end_scope) {
				bb_fcall_end_scope = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(bb_fcall_end_scope);
			if (bb_follow) {
				llvm_ctx.builder.SetInsertPoint(bb_follow);
			}
		}
		if (!func || (func->common.fn_flags & ZEND_ACC_GENERATOR) == 0) {
			//JIT: call->prev_execute_data = execute_data;
			llvm_ctx.builder.CreateAlignedStore(
				llvm_ctx._execute_data,
				zend_jit_GEP(
					llvm_ctx,
					call,
					offsetof(zend_execute_data, prev_execute_data),
					PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.zend_execute_data_type))), 4);
			//JIT: i_init_func_execute_data(call, &fbc->op_array, return_value, VM_FRAME_TOP_FUNCTION TSRMLS_CC);
			Value *obj = object ? 
				object :
				llvm_ctx.builder.CreateIntToPtr(
					LLVM_GET_LONG(0),
					PointerType::getUnqual(llvm_ctx.zend_object_type));
			zend_jit_init_func_execute_data(llvm_ctx, call, obj, func, func_addr, return_value, num_args, opline->lineno);

			if (ZEND_LLVM_MODULE_AT_ONCE &&
			    func_info &&
			    (func_info->flags & ZEND_JIT_FUNC_MAY_COMPILE)) {
//???				if (func_info->flags & ZEND_JIT_FUNC_INLINE) {
//???					if (!zend_jit_inline(ctx, llvm_ctx, &func->op_array, opline, ex)) {
//???						return 0;
//???					}
//???				} else {
					Function *_helper = zend_jit_get_func(llvm_ctx, ctx, &func->op_array, func_info);
					CallInst *inst = llvm_ctx.builder.CreateCall(_helper, call);
					inst->setCallingConv(CallingConv::X86_FastCall);
//???				}
			} else {
				//JIT: zend_execute_ex(call TSRMLS_CC);
				Function *_helper = zend_jit_get_helper(
					llvm_ctx,
					(void*)orig_execute_ex,
					ZEND_JIT_SYM("execute_ex"),
					0,
					Type::getVoidTy(llvm_ctx.context),
					PointerType::getUnqual(llvm_ctx.zend_execute_data_type),
					NULL,
					NULL,
					NULL,
					NULL);
				llvm_ctx.builder.CreateCall(_helper, call);
			}

			if (!bb_fcall_end_scope) {
				bb_fcall_end_scope = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			}
			llvm_ctx.builder.CreateBr(bb_fcall_end_scope);
		}
		if (bb_follow) {
			llvm_ctx.builder.SetInsertPoint(bb_follow);
		}
	}

	if (!func || func->type == ZEND_OVERLOADED_FUNCTION) {
		//???... overloaded function
		if (!bb_fcall_end_scope) {
			bb_fcall_end_scope = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		}
		llvm_ctx.builder.CreateBr(bb_fcall_end_scope);
	}

	if (bb_fcall_end_scope) {
		llvm_ctx.builder.SetInsertPoint(bb_fcall_end_scope);
//???..check
		if (object) {
			//JIT: if (object) {
			BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			BasicBlock *bb_end = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			zend_jit_unexpected_br(llvm_ctx,
				llvm_ctx.builder.CreateIsNotNull(object),
				bb_follow,
				bb_end);
			llvm_ctx.builder.SetInsertPoint(bb_follow);
			if (OP1_OP()->num & ZEND_CALL_CTOR) {
				//JIT: if (UNEXPECTED(EG(exception) != NULL) {
				BasicBlock *bb_skip = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	    		BasicBlock *bb_exception = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
    			BasicBlock *bb_ctor_failed = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

				zend_jit_unexpected_br(llvm_ctx,
					llvm_ctx.builder.CreateIsNotNull(
						llvm_ctx.builder.CreateAlignedLoad(llvm_ctx._EG_exception, 4, 1)),
					bb_exception,
					bb_skip);
				llvm_ctx.builder.SetInsertPoint(bb_exception);

				if (!(OP1_OP()->num & ZEND_CALL_CTOR_RESULT_UNUSED)) {
					//JIT: GC_REFCOUNT(object)--;
					zend_jit_delref(llvm_ctx, object);
				}
				//JIT: if (GC_REFCOUNT(object) == 1) {
				zend_jit_unexpected_br(llvm_ctx,				
					llvm_ctx.builder.CreateICmpEQ(
						llvm_ctx.builder.CreateAlignedLoad(
							zend_jit_refcount_addr(llvm_ctx, object), 4),
						llvm_ctx.builder.getInt32(1)),
					bb_ctor_failed,
					bb_skip);
				llvm_ctx.builder.SetInsertPoint(bb_ctor_failed);
				//JIT: zend_object_store_ctor_failed(object TSRMLS_CC);
				zend_jit_object_store_ctor_failed(llvm_ctx, object);
				llvm_ctx.builder.CreateBr(bb_skip);
				llvm_ctx.builder.SetInsertPoint(bb_skip);
			}
			//JIT: OBJ_RELEASE(object);
			zend_jit_object_release(llvm_ctx, object, opline->lineno);
			llvm_ctx.builder.CreateBr(bb_end);
			llvm_ctx.builder.SetInsertPoint(bb_end);
		}
		if (!func ||
		    func->op_array.type != ZEND_USER_FUNCTION ||
		    op_array->type != ZEND_USER_FUNCTION ||
		    !op_array->function_name ||
		    op_array->scope != func->op_array.scope) {
			//JIT: EG(scope) = EX(func)->common.scope;
			llvm_ctx.builder.CreateAlignedStore(
				llvm_ctx.builder.CreateAlignedLoad(
					zend_jit_GEP(
						llvm_ctx,
						llvm_ctx.builder.CreateAlignedLoad(
							zend_jit_GEP(
								llvm_ctx,
								llvm_ctx._execute_data,
								offsetof(zend_execute_data, func),
								PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.zend_function_type))), 4),
						offsetof(zend_function, common.scope),
						PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.zend_class_entry_type))), 4),
				llvm_ctx._EG_scope, 4);
		}
		if (bb_fcall_end) {
			llvm_ctx.builder.CreateBr(bb_fcall_end);
		}
	}

	if (bb_fcall_end) {
		llvm_ctx.builder.SetInsertPoint(bb_fcall_end);
	}
	do {
		//JIT: if (UNEXPECTED(EG(exception) != NULL)) {
		BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
    	BasicBlock *bb_exception = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

		zend_jit_unexpected_br(llvm_ctx,
			llvm_ctx.builder.CreateIsNotNull(
				llvm_ctx.builder.CreateAlignedLoad(llvm_ctx._EG_exception, 4, 1)),
			bb_exception,
			bb_follow);

		llvm_ctx.builder.SetInsertPoint(bb_exception);
		//JIT: zend_throw_exception_internal(NULL TSRMLS_CC);
		zend_jit_throw_exception_internal(llvm_ctx,
			llvm_ctx.builder.CreateIntToPtr(
				LLVM_GET_LONG(0),
				llvm_ctx.zval_ptr_type));
		if (RETURN_VALUE_USED(opline)) {
			//JIT: zval_ptr_dtor(EX_VAR(RES_OP()->var));
			Value *ret = zend_jit_load_var(llvm_ctx, RES_OP()->var);
			zend_jit_zval_ptr_dtor_ex(llvm_ctx, ret, NULL, RES_SSA_VAR(), RES_INFO(), opline->lineno, 1); 
		}
		//JIT: HANDLE_EXCEPTION();
		llvm_ctx.builder.CreateBr(zend_jit_find_exception_bb(llvm_ctx, opline));
		llvm_ctx.builder.SetInsertPoint(bb_follow);
	} while (0);

	//JIT: ZEND_VM_NEXT_OPCODE();
	llvm_ctx.valid_opline = 0;
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_free_compiled_variables */
static int zend_jit_free_compiled_variables(zend_llvm_ctx    &llvm_ctx,
                                            zend_op_array    *op_array,
                                            zend_op          *opline)
{
	zend_jit_func_info *ctx = JIT_DATA(op_array);
    uint32_t info;

	// TODO: use type inference to avoid useless zval_ptr_dtor() ???...
	for (uint32_t i = 0 ; i < op_array->last_var; i++) {	    
	    if (ctx && ctx->ssa.var && ctx->ssa_var_info) {
			info = ctx->ssa_var_info[i].type;
		    for (uint32_t j = op_array->last_var; j < ctx->ssa.vars; j++) {
		    	if (ctx->ssa.var[j].var == i) {
		    		if (!(ctx->ssa_var_info[j].type & MAY_BE_IN_REG)) {
						info |= ctx->ssa_var_info[j].type;
					}
				}
			}
		} else {
			info = MAY_BE_RC1 | MAY_BE_RCN | MAY_BE_REF | MAY_BE_ANY;
		}

		if (info & (MAY_BE_STRING|MAY_BE_ARRAY|MAY_BE_OBJECT|MAY_BE_RESOURCE|MAY_BE_REF)) {
			Value *var = zend_jit_GEP(
				llvm_ctx,
				llvm_ctx._execute_data,
				(ZEND_CALL_FRAME_SLOT + i) * sizeof(zval),
				llvm_ctx.zval_ptr_type);
			zend_jit_zval_ptr_dtor_ex(llvm_ctx, var, NULL, -1, info, opline->lineno, 1);
		}
	}
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_free_extra_args */
static int zend_jit_free_extra_args_helper(zend_llvm_ctx &llvm_ctx,
                                           Value         *execute_data)
{
	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)zend_jit_helper_free_extra_args,
			ZEND_JIT_SYM("zend_jit_helper_free_extra_args"),
			ZEND_JIT_HELPER_FAST_CALL,
			Type::getVoidTy(llvm_ctx.context),
			PointerType::getUnqual(llvm_ctx.zend_execute_data_type),
			NULL,
			NULL,
			NULL,
			NULL);

	CallInst *call = llvm_ctx.builder.CreateCall(_helper, execute_data);
	call->setCallingConv(CallingConv::X86_FastCall);
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_free_extra_args */
static int zend_jit_free_extra_args(zend_llvm_ctx    &llvm_ctx,
                                    zend_op_array    *op_array,
                                    zend_op          *opline)
{
	uint32_t first_extra_arg = op_array->num_args;
	
	//JIT: if (UNEXPECTED(ZEND_CALL_NUM_ARGS(call) > first_extra_arg)) {
	Value *num_args = 
		llvm_ctx.builder.CreateAlignedLoad(
			zend_jit_GEP(
				llvm_ctx,
				llvm_ctx._execute_data,
				offsetof(zend_execute_data, This.u2.num_args),
				PointerType::getUnqual(Type::getInt32Ty(llvm_ctx.context))), 4);
   	BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
   	BasicBlock *bb_end = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	zend_jit_unexpected_br(llvm_ctx,
		llvm_ctx.builder.CreateICmpUGT(
			num_args,
			llvm_ctx.builder.getInt32(first_extra_arg)),
		bb_follow,
		bb_end);
	llvm_ctx.builder.SetInsertPoint(bb_follow);
#if 1
	zend_jit_free_extra_args_helper(llvm_ctx, llvm_ctx._execute_data);
	llvm_ctx.builder.CreateBr(bb_end);
#else
	//JIT: zval *end = ZEND_CALL_VAR_NUM(call, call->func->op_array.last_var + call->func->op_array.T);
	Value *end = zend_jit_GEP(
		llvm_ctx,
		llvm_ctx._execute_data,
		(ZEND_CALL_FRAME_SLOT + op_array->last_var + op_array->T) * sizeof(zval),
		llvm_ctx.zval_ptr_type);
	//JIT:  zval *p = end + (call->num_args - first_extra_arg);
	Value *var = llvm_ctx.builder.CreateInBoundsGEP(
		end,
		llvm_ctx.builder.CreateSub(
			num_args,
			llvm_ctx.builder.getInt32(first_extra_arg)));
   	BasicBlock *bb_loop = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	//JIT: do {
	PHI_DCL2(var, 2);
	PHI_ADD2(var, var);
	llvm_ctx.builder.CreateBr(bb_loop);
	llvm_ctx.builder.SetInsertPoint(bb_loop);
	PHI_SET2(var, var, llvm_ctx.zval_ptr_type);
	//JIT: p--;
	var = zend_jit_GEP(
		llvm_ctx,
		var,
		-sizeof(zval),
		llvm_ctx.zval_ptr_type);
	//JIT: zval_ptr_dtor_nogc(p);
	zend_jit_zval_ptr_dtor_ex(llvm_ctx, var, NULL, -1, MAY_BE_ANY, opline->lineno, 0);
	//JIT: } while (var != end);
	PHI_ADD2(var, var);
	zend_jit_expected_br(llvm_ctx,
		llvm_ctx.builder.CreateICmpNE(
			var,
			end),
		bb_loop,
		bb_end);
#endif
	llvm_ctx.builder.SetInsertPoint(bb_end);
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_clean_and_cache_symbol_table */
static int zend_jit_clean_and_cache_symbol_table(zend_llvm_ctx    &llvm_ctx,
                                                 Value            *symbol_table)
{
	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)zend_clean_and_cache_symbol_table,
			ZEND_JIT_SYM("zend_jit_clean_and_cache_symbol_table"),
			0,
			llvm_ctx.zval_ptr_type,
			PointerType::getUnqual(llvm_ctx.zend_array_type),
			NULL,
			NULL,
			NULL);

	llvm_ctx.builder.CreateCall(
		_helper, symbol_table);

	return 1;
}
/* }}} */

/* {{{ static int zend_jit_leave */
static int zend_jit_leave(zend_llvm_ctx    &llvm_ctx,
                          zend_op_array    *op_array,
                          zend_op          *opline)
{
	zend_jit_func_info *info = JIT_DATA(op_array);

	if (llvm_ctx.bb_leave) {
		llvm_ctx.builder.CreateBr(llvm_ctx.bb_leave);
		return 1;
	} else {
		llvm_ctx.bb_leave = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);		
		llvm_ctx.builder.CreateBr(llvm_ctx.bb_leave);
		llvm_ctx.builder.SetInsertPoint(llvm_ctx.bb_leave);
	}
	//JIT: i_free_compiled_variables(execute_data TSRMLS_CC);
	if (!zend_jit_free_compiled_variables(llvm_ctx, op_array, opline)) return 0;
	if ((info->flags & ZEND_JIT_FUNC_NO_SYMTAB) == 0) {
		//JIT: if (UNEXPECTED(EX(symbol_table) != NULL)) {
		Value *symbol_table = 
			llvm_ctx.builder.CreateAlignedLoad(
				zend_jit_GEP(
					llvm_ctx,
					llvm_ctx._execute_data,
					offsetof(zend_execute_data, symbol_table),
					PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.zend_array_type))), 4);
	   	BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
   		BasicBlock *bb_skip = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		zend_jit_unexpected_br(llvm_ctx,
			llvm_ctx.builder.CreateIsNotNull(symbol_table),
			bb_follow,
			bb_skip);
		llvm_ctx.builder.SetInsertPoint(bb_follow);
		//JIT: zend_clean_and_cache_symbol_table(EX(symbol_table) TSRMLS_CC);
		zend_jit_clean_and_cache_symbol_table(llvm_ctx, symbol_table);
		llvm_ctx.builder.CreateBr(bb_skip);
		llvm_ctx.builder.SetInsertPoint(bb_skip);
	}
	//JIT: zend_vm_stack_free_extra_args(execute_data TSRMLS_CC);
	zend_jit_free_extra_args(llvm_ctx, op_array, opline);
	//JIT: EG(current_execute_data) = EX(prev_execute_data);
	llvm_ctx.builder.CreateAlignedStore(
		llvm_ctx.builder.CreateAlignedLoad(
			zend_jit_GEP(
				llvm_ctx,
				llvm_ctx._execute_data,
				offsetof(zend_execute_data, prev_execute_data),
				PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.zend_execute_data_type))), 4),
		llvm_ctx._EG_current_execute_data, 4);
	if (op_array->fn_flags & ZEND_ACC_CLOSURE) {
		//JIT: if (EX(func)->op_array.prototype) {
		Value *func = 
			llvm_ctx.builder.CreateAlignedLoad(
				zend_jit_GEP(
					llvm_ctx,
					llvm_ctx._execute_data,
					offsetof(zend_execute_data, func),
					PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.zend_function_type))), 4);
		Value *object = 
			llvm_ctx.builder.CreateAlignedLoad(
				zend_jit_GEP(
					llvm_ctx,
					func,
					offsetof(zend_function, op_array.prototype),
					PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.zend_object_type))), 4);
	   	BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
   		BasicBlock *bb_skip = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		zend_jit_expected_br(llvm_ctx,
			llvm_ctx.builder.CreateIsNotNull(object),
			bb_follow,
			bb_skip);
		llvm_ctx.builder.SetInsertPoint(bb_follow);
		//JIT: OBJ_RELEASE((zend_object*)EX(func)->op_array.prototype);
		zend_jit_object_release(llvm_ctx, object, opline->lineno);
		llvm_ctx.builder.CreateBr(bb_skip);
		llvm_ctx.builder.SetInsertPoint(bb_skip);
	}
	//JIT: zend_vm_stack_free_call_frame(execute_data TSRMLS_CC);
	zend_jit_vm_stack_free_call_frame(llvm_ctx, llvm_ctx._execute_data, opline->lineno);

	llvm_ctx.builder.CreateRet(llvm_ctx.builder.getInt32(-1));
	llvm_ctx.valid_opline = 0;
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_return */
static int zend_jit_return(zend_llvm_ctx    &llvm_ctx,
                           zend_op_array    *op_array,
                           zend_op          *opline)
{
	if (op_array->type == ZEND_EVAL_CODE || !op_array->function_name) {
//TODO: ???
		return zend_jit_tail_handler(llvm_ctx, opline);
	}
	
	//JIT: retval_ptr = GET_OP1_ZVAL_PTR(BP_VAR_R);
	Value *retval = zend_jit_load_operand(llvm_ctx,
			OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO(), 0, opline);

	//JIT: if (!EX(return_value)) {
	BasicBlock *bb_noret = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
   	BasicBlock *bb_ret = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
   	BasicBlock *bb_end = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

	Value *return_value = llvm_ctx.builder.CreateAlignedLoad(
		zend_jit_GEP(
			llvm_ctx,
			llvm_ctx._execute_data,
			offsetof(zend_execute_data, return_value),
			PointerType::getUnqual(llvm_ctx.zval_ptr_type)), 4);
	zend_jit_unexpected_br(llvm_ctx,
		llvm_ctx.builder.CreateIsNull(return_value),
		bb_noret,
		bb_ret);
	llvm_ctx.builder.SetInsertPoint(bb_noret);
	//JIT: FREE_OP1();
	if (!zend_jit_free_operand(llvm_ctx, OP1_OP_TYPE(), retval, NULL, OP1_SSA_VAR(), OP1_INFO(), opline->lineno)) {
		return 0;
	}
	llvm_ctx.builder.CreateBr(bb_end);
	llvm_ctx.builder.SetInsertPoint(bb_ret);
	if (OP1_OP_TYPE() == IS_CONST || OP1_OP_TYPE() == IS_TMP_VAR) {
		//JIT: ZVAL_COPY_VALUE(EX(return_value), retval_ptr);
		zend_jit_copy_value(llvm_ctx, return_value, 0, -1, MAY_BE_ANY,
				retval, NULL, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO());
		if (OP1_OP_TYPE() == IS_CONST) {
			if (Z_OPT_COPYABLE_P(RT_CONSTANT(llvm_ctx.op_array, *OP1_OP()))) {
				//JIT: zval_copy_ctor_func(EX(return_value));
				zend_jit_copy_ctor_func(llvm_ctx, return_value, opline->lineno);
			}
		}
		llvm_ctx.builder.CreateBr(bb_end);
	} else {
		Value *retval_type_info = NULL;
		BasicBlock *bb_no_ref = NULL;
		if (OP1_MAY_BE(MAY_BE_REF)) {
			if (OP1_MAY_BE(MAY_BE_RC1|MAY_BE_RCN)) {
				//JIT: if (Z_ISREF_P(retval_ptr)) {
				retval_type_info = zend_jit_load_type_info_c(llvm_ctx, retval, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO());
				BasicBlock *bb_ref = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				bb_no_ref = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				zend_jit_unexpected_br(llvm_ctx,
					llvm_ctx.builder.CreateICmpEQ(
						retval_type_info,
						llvm_ctx.builder.getInt32(IS_REFERENCE_EX)),
					bb_ref,
					bb_no_ref);
				llvm_ctx.builder.SetInsertPoint(bb_ref);
			}
			//JIT: ZVAL_COPY(EX(return_value), Z_REFVAL_P(retval_ptr));
			Value *counted = zend_jit_load_counted(llvm_ctx, retval, OP1_SSA_VAR(), OP1_INFO());
			Value *ref_addr = zend_jit_load_reference(llvm_ctx, counted);
			Value *ref_type_info = zend_jit_load_type_info(llvm_ctx, ref_addr, -1, MAY_BE_ANY);
			zend_jit_copy_value(llvm_ctx, return_value,0, -1, MAY_BE_ANY,
				ref_addr, ref_type_info, OP1_OP_TYPE(), OP1_OP(), -1, OP1_INFO() & MAY_BE_ANY);
			zend_jit_try_addref(llvm_ctx, return_value, ref_type_info, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO());
			if (OP1_OP_TYPE() == IS_VAR) {
				//JIT: FREE_OP1_IF_VAR();
				if (!zend_jit_free_operand(llvm_ctx, OP1_OP_TYPE(), retval, NULL, OP1_SSA_VAR(), OP1_INFO(), opline->lineno)) {
				   	return 0;
				}
			}
			llvm_ctx.builder.CreateBr(bb_end);
		}
		if (OP1_MAY_BE(MAY_BE_RC1|MAY_BE_RCN)) {
			if (bb_no_ref) {
				llvm_ctx.builder.SetInsertPoint(bb_no_ref);
			}
			//JIT: ZVAL_COPY_VALUE(EX(return_value), retval_ptr);
			zend_jit_copy_value(llvm_ctx, return_value, 0, -1, MAY_BE_ANY,
				retval, retval_type_info, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO());
			if (OP1_OP_TYPE() == IS_CV) {
				//JIT: if (Z_OPT_REFCOUNTED_P(retval_ptr)) Z_ADDREF_P(retval_ptr);
				zend_jit_try_addref(llvm_ctx, return_value, retval_type_info, OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO());
			}
			llvm_ctx.builder.CreateBr(bb_end);
		}
	}
	llvm_ctx.builder.SetInsertPoint(bb_end);

	//JIT: ZEND_VM_DISPATCH_TO_HELPER(zend_leave_helper);
	return zend_jit_leave(llvm_ctx, op_array, opline);
}
/* }}} */

/* {{{ static int zend_jit_recv */
static int zend_jit_recv(zend_llvm_ctx    &llvm_ctx,
                         zend_op_array    *op_array,
                         zend_op          *opline)
{
	llvm_ctx.valid_opline = 0;

	BasicBlock *bb_miss = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
  	BasicBlock *bb_check = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	BasicBlock *bb_end = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		
	//JIT: param = _get_zval_ptr_cv_undef_BP_VAR_W(execute_data, RES_OP()->var TSRMLS_CC);
	Value *param = zend_jit_load_slot(llvm_ctx, RES_OP()->var);

	//JIT: if (UNEXPECTED(arg_num > EX_NUM_ARGS())) {
	Value *num_args = llvm_ctx.builder.CreateAlignedLoad(
		zend_jit_GEP(
			llvm_ctx,
			llvm_ctx._execute_data,
			offsetof(zend_execute_data, This.u2.num_args),
			PointerType::getUnqual(Type::getInt32Ty(llvm_ctx.context))), 4);
	zend_jit_unexpected_br(llvm_ctx,
		llvm_ctx.builder.CreateICmpUGT(
			llvm_ctx.builder.getInt32(OP1_OP()->num),
			num_args),
		bb_miss,
		bb_check);
		
	llvm_ctx.builder.SetInsertPoint(bb_miss);

	if (opline->opcode == ZEND_RECV_INIT) {
		//JIT: ZVAL_COPY_VALUE(param, OP2_OP()->zv);
		zend_jit_copy_value(llvm_ctx, param, 0, -1, MAY_BE_ANY,
			llvm_ctx.builder.CreateIntToPtr(
				LLVM_GET_LONG((zend_uintptr_t)RT_CONSTANT(llvm_ctx.op_array, *OP2_OP())),
				llvm_ctx.zval_ptr_type),
			NULL,
			OP2_OP_TYPE(),
			OP2_OP(),
			OP2_SSA_VAR(),
			OP2_INFO());
		if (Z_OPT_CONSTANT_P(RT_CONSTANT(llvm_ctx.op_array, *OP2_OP()))) {
			BasicBlock *bb_error = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		  	BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);

			//JIT: SAVE_OPLINE();
			if (!llvm_ctx.valid_opline) {
				JIT_CHECK(zend_jit_store_opline(llvm_ctx, opline, false));
			}
			//JIT: if (UNEXPECTED(zval_update_constant_ex(param, 0, NULL) != SUCCESS)) {
			Value *ret = zend_jit_update_constant(llvm_ctx, param, 0);
			zend_jit_unexpected_br(
					llvm_ctx,
					llvm_ctx.builder.CreateICmpNE(ret, llvm_ctx.builder.getInt32(SUCCESS)),
					bb_error,
					bb_follow);

			llvm_ctx.builder.SetInsertPoint(bb_error);
			//JIT: ZVAL_UNDEF(param);
			zend_jit_save_zval_type_info(llvm_ctx, param, -1, MAY_BE_ANY, llvm_ctx.builder.getInt32(IS_UNDEF));
			//JIT: HANDLE_EXCEPTION();
			llvm_ctx.builder.CreateBr(zend_jit_find_exception_bb(llvm_ctx, opline));
			llvm_ctx.builder.SetInsertPoint(bb_follow);
		} else {
			/* IS_CONST can't be IS_OBJECT, IS_RESOURCE or IS_REFERENCE */
			if (Z_OPT_COPYABLE_P(RT_CONSTANT(llvm_ctx.op_array, *OP2_OP()))) {
				//JIT: zval_copy_ctor_func(param);
				zend_jit_copy_ctor_func(llvm_ctx, param, opline->lineno);
			}
		}
		llvm_ctx.builder.CreateBr(bb_check);
	} else if (opline->opcode == ZEND_RECV) {
		//JIT: SAVE_OPLINE();
		if (!llvm_ctx.valid_opline) {
			JIT_CHECK(zend_jit_store_opline(llvm_ctx, opline, false));
		}
		//JIT: zend_verify_missing_arg(execute_data, arg_num, CACHE_ADDR(opline->op2.num));
		zend_jit_check_missing_arg(llvm_ctx,
			OP1_OP()->num,
			zend_jit_cache_slot_addr(
				llvm_ctx,
				opline->op2.num,
				PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))));
		//JIT: CHECK_EXCEPTION();
		JIT_CHECK(zend_jit_check_exception(llvm_ctx, opline));
		llvm_ctx.builder.CreateBr(bb_end);
	} else {
		ASSERT_NOT_REACHED();
	}
	llvm_ctx.builder.SetInsertPoint(bb_check);
	if ((op_array->fn_flags & ZEND_ACC_HAS_TYPE_HINTS) != 0) {
		uint32_t arg_num = OP1_OP()->num - 1;
		zend_arg_info *cur_arg_info;
		do {
			if (arg_num < op_array->num_args) {
				cur_arg_info = &op_array->arg_info[arg_num];
			} else if (op_array->fn_flags & ZEND_ACC_VARIADIC) {
				cur_arg_info = &op_array->arg_info[op_array->num_args];
			} else {
				break;
			}
			if (cur_arg_info->class_name || cur_arg_info->type_hint) {
				//JIT: SAVE_OPLINE();
				if (!llvm_ctx.valid_opline) {
					JIT_CHECK(zend_jit_store_opline(llvm_ctx, opline, false));
				}
				//JIT: if (UNEXPECTED(!zend_verify_arg_type(EX(func), arg_num, param, default_value, CACHE_ADDR(Z_CACHE_SLOT_P(default_value))))) {
				Value *ret = zend_jit_check_type_hint(
					llvm_ctx,
					llvm_ctx.builder.CreateIntToPtr(
						LLVM_GET_LONG((zend_uintptr_t)op_array),
						PointerType::getUnqual(llvm_ctx.zend_function_type)),
					llvm_ctx.builder.getInt32(arg_num + 1),
					param,
					llvm_ctx.builder.CreateIntToPtr(
						(opline->opcode == ZEND_RECV_INIT) ?
							LLVM_GET_LONG((zend_uintptr_t)RT_CONSTANT(llvm_ctx.op_array, opline->op2)) :
							LLVM_GET_LONG(0),
						llvm_ctx.zval_ptr_type),
					zend_jit_cache_slot_addr(
						llvm_ctx,
						(opline->opcode == ZEND_RECV_INIT) ?
							Z_CACHE_SLOT_P(RT_CONSTANT(llvm_ctx.op_array, *OP2_OP())) :
							opline->op2.num,
						PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context))));
				BasicBlock *bb_error = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				BasicBlock *bb_follow = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				zend_jit_unexpected_br(
					llvm_ctx,
					llvm_ctx.builder.CreateICmpEQ(ret, llvm_ctx.builder.getInt32(0)),
					bb_error,
					bb_follow);
				llvm_ctx.builder.SetInsertPoint(bb_error);
				//JIT: HANDLE_EXCEPTION();
				llvm_ctx.builder.CreateBr(zend_jit_find_exception_bb(llvm_ctx, opline));
				llvm_ctx.builder.SetInsertPoint(bb_follow);
			}
		} while (0);
	}
	llvm_ctx.builder.CreateBr(bb_end);
	llvm_ctx.builder.SetInsertPoint(bb_end);
	//JIT: ZEND_VM_NEXT_OPCODE();
	llvm_ctx.valid_opline = 0;
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_write */
static int zend_jit_write(zend_llvm_ctx    &llvm_ctx,
                          Value            *str,
                          Value            *len)
{
	Function *helper = zend_jit_get_helper(
		llvm_ctx,
		(void*)zend_write,
		ZEND_JIT_SYM("zend_write"),
		ZEND_JIT_HELPER_ARG1_NOALIAS | ZEND_JIT_HELPER_ARG1_NOCAPTURE,
		Type::getVoidTy(llvm_ctx.context),
		PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context)),
		LLVM_GET_LONG_TY(llvm_ctx.context),
		NULL,
		NULL,
		NULL);
	llvm_ctx.builder.CreateCall2(helper, str, len);
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_echo */
static int zend_jit_echo(zend_llvm_ctx    &llvm_ctx,
                         zend_op_array    *op_array,
                         zend_op          *opline)
{
	if (opline->op1_type == IS_CONST && Z_TYPE_P(RT_CONSTANT(llvm_ctx.op_array, opline->op1)) == IS_STRING) {
		if (Z_STRLEN_P(RT_CONSTANT(llvm_ctx.op_array, opline->op1)) > 0) {
			//JIT: zend_write(Z_STRVAL_P(z), Z_STRLEN_P(z));
			zend_jit_write(llvm_ctx,
				LLVM_GET_CONST_STRING(Z_STRVAL_P(RT_CONSTANT(llvm_ctx.op_array, opline->op1))),
				LLVM_GET_LONG(Z_STRLEN_P(RT_CONSTANT(llvm_ctx.op_array, opline->op1))));
				
		}
	} else {
		BasicBlock *bb_convert = NULL;
		BasicBlock *bb_common = NULL;

	    //JIT: z = GET_OP1_ZVAL_PTR_DEREF(BP_VAR_R);
		Value *op1_addr = zend_jit_load_operand(llvm_ctx,
			OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO(), 0, opline);

		if (OP1_MAY_BE(MAY_BE_STRING)) {
			if (OP1_MAY_BE(MAY_BE_ANY - MAY_BE_STRING)) {
				BasicBlock *bb_string = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				bb_convert = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				bb_common = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
				zend_jit_expected_br(llvm_ctx,
						llvm_ctx.builder.CreateICmpEQ(
							zend_jit_load_type(llvm_ctx, op1_addr, OP1_SSA_VAR(), OP1_INFO()),
							llvm_ctx.builder.getInt8(IS_STRING)),
						bb_string,
						bb_convert);
				llvm_ctx.builder.SetInsertPoint(bb_string);
			}
			//JIT: str = Z_STR_P(z);
			Value *str = zend_jit_load_str(llvm_ctx, op1_addr, OP1_SSA_VAR(), OP1_INFO());
			//JIT: zend_write(str->val, str->len);
			zend_jit_write(llvm_ctx,
				zend_jit_load_str_val(llvm_ctx, str),
				zend_jit_load_str_len(llvm_ctx, str));
			if (bb_common) {
				llvm_ctx.builder.CreateBr(bb_common);
			}
		}	
	
		if (OP1_MAY_BE(MAY_BE_ANY - MAY_BE_STRING)) {
			if (bb_convert) {
				llvm_ctx.builder.SetInsertPoint(bb_convert);
			}
			if (!llvm_ctx.valid_opline) {
				JIT_CHECK(zend_jit_store_opline(llvm_ctx, opline, false));
			}
			if (OP1_MAY_BE(MAY_BE_IN_REG)) {
				op1_addr = zend_jit_reload_from_reg(llvm_ctx, OP1_SSA_VAR(), OP1_INFO());
			}
			//JIT: str = zval_get_string(expr);
			Value *str = zend_jit_zval_get_string_func(llvm_ctx, op1_addr);
			//JIT: zend_write(str->val, str->len);
			zend_jit_write(llvm_ctx,
				zend_jit_load_str_val(llvm_ctx, str),
				zend_jit_load_str_len(llvm_ctx, str));
			//JIT: zend_string_release(str);
			zend_jit_string_release(llvm_ctx, str);
			if (bb_common) {
				llvm_ctx.builder.CreateBr(bb_common);
			}
		}
		if (bb_common) {
			llvm_ctx.builder.SetInsertPoint(bb_common);
		}

		if (!zend_jit_free_operand(llvm_ctx, opline->op1_type, op1_addr, NULL, OP1_SSA_VAR(), OP1_INFO(), opline->lineno)) return 0;
	}

//???	if (may_threw) {
		JIT_CHECK(zend_jit_check_exception(llvm_ctx, opline));
//???	}

	//JIT: ZEND_VM_NEXT_OPCODE();
	llvm_ctx.valid_opline = 0;
	return 1;
}
/* }}} */

/* {{{ static int zend_jit_free */
static int zend_jit_free(zend_llvm_ctx    &llvm_ctx,
                         zend_op_array    *op_array,
                         zend_op          *opline)
{
	Value *op1_addr = zend_jit_load_operand(llvm_ctx,
			OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO(), 0, opline);

	if (!zend_jit_free_operand(llvm_ctx, opline->op1_type, op1_addr, NULL, OP1_SSA_VAR(), OP1_INFO(), opline->lineno)) return 0;

	if (OP1_MAY_BE(MAY_BE_OBJECT|MAY_BE_RESOURCE|MAY_BE_ARRAY_OF_OBJECT|MAY_BE_ARRAY_OF_RESOURCE)) {
		JIT_CHECK(zend_jit_check_exception(llvm_ctx, opline));
	}

	//JIT: ZEND_VM_NEXT_OPCODE();
	llvm_ctx.valid_opline = 0;
	return 1;
}
/* }}} */

/* {{{ static Value* zend_jit_rsrc_list_get_rsrc_type */
static Value* zend_jit_rsrc_list_get_rsrc_type(zend_llvm_ctx    &llvm_ctx,
                                               Value            *res)
{
	Function *_helper = zend_jit_get_helper(
			llvm_ctx,
			(void*)zend_rsrc_list_get_rsrc_type,
			ZEND_JIT_SYM("zend_rsrc_list_get_rsrc_type"),
			0,
			PointerType::getUnqual(Type::getInt8Ty(llvm_ctx.context)),
			PointerType::getUnqual(llvm_ctx.zend_res_type),
			NULL,
			NULL,
			NULL,
			NULL);

	return llvm_ctx.builder.CreateCall(_helper, res);
}
/* }}} */

/* {{{ static int zend_jit_type_check */
static int zend_jit_type_check(zend_llvm_ctx    &llvm_ctx,
                               zend_op_array    *op_array,
                               zend_op          *opline)
{
	BasicBlock *bb_false = NULL;
	BasicBlock *bb_true = NULL;
	BasicBlock *bb_next = NULL;
	Value *res;
	Value *op1_addr;
	Value *op1_type;
	Value *orig_op1_addr = zend_jit_load_operand(llvm_ctx,
			OP1_OP_TYPE(), OP1_OP(), OP1_SSA_VAR(), OP1_INFO(), 0, opline);

	//JIT: value = GET_OP1_ZVAL_PTR_DEREF(BP_VAR_R);
	if (opline->op1_type == IS_VAR || opline->op1_type == IS_CV) {
		op1_addr = zend_jit_deref(llvm_ctx, orig_op1_addr, OP1_SSA_VAR(), OP1_INFO());
	} else {
		op1_addr = orig_op1_addr;
	}
	op1_type =	zend_jit_load_type(llvm_ctx, op1_addr, OP1_SSA_VAR(), OP1_INFO()),

	res = zend_jit_load_tmp_zval(llvm_ctx, opline->result.var);

	switch (opline->extended_value) {
		case IS_NULL:
		case IS_LONG:
		case IS_DOUBLE:
		case IS_STRING:
		case IS_ARRAY:
			//JIT: ZVAL_BOOL(EX_VAR(opline->result.var), Z_TYPE_P(value) == opline->extended_value);
			zend_jit_save_zval_type_info(llvm_ctx,
				res,
				RES_SSA_VAR(),
				RES_INFO(),
				llvm_ctx.builder.CreateAdd(
					llvm_ctx.builder.CreateZExtOrBitCast(
						llvm_ctx.builder.CreateICmpEQ(
							op1_type,
							llvm_ctx.builder.getInt8(opline->extended_value)),
						Type::getInt32Ty(llvm_ctx.context)),
					llvm_ctx.builder.getInt32(IS_FALSE)));
			break;
		case _IS_BOOL:
			//JIT: ZVAL_BOOL(EX_VAR(opline->result.var), Z_TYPE_P(value) == IS_TRUE || Z_TYPE_P(value) == IS_FALSE);
			bb_false = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_true = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_next = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			zend_jit_unexpected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(
					op1_type,
					llvm_ctx.builder.getInt8(IS_FALSE)),
				bb_true,
				bb_next);
			llvm_ctx.builder.SetInsertPoint(bb_next);
			zend_jit_unexpected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(
					op1_type,
					llvm_ctx.builder.getInt8(IS_TRUE)),
				bb_true,
				bb_false);
			break;
		case IS_OBJECT: {
			//JIT: if (Z_TYPE_P(value) == opline->extended_value) {
			bb_false = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_true = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_next = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			zend_jit_expected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(
					op1_type,
					llvm_ctx.builder.getInt8(IS_OBJECT)),
				bb_next,
				bb_false);
			llvm_ctx.builder.SetInsertPoint(bb_next);
			//JIT: zend_class_entry *ce = Z_OBJCE_P(value);
			Value *obj = zend_jit_load_obj(llvm_ctx, op1_addr, OP1_SSA_VAR(), OP1_INFO());
			Value *ce = zend_jit_load_obj_ce(llvm_ctx, obj);
			Value *name = zend_jit_load_ce_name(llvm_ctx, ce);
			Value *len = zend_jit_load_str_len(llvm_ctx, name);
			//JIT:  if (ce->name->len == sizeof("__PHP_Incomplete_Class") - 1
			bb_next = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			zend_jit_expected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(
					len,
					LLVM_GET_LONG(sizeof("__PHP_Incomplete_Class") - 1)),
				bb_next,
				bb_true);
			llvm_ctx.builder.SetInsertPoint(bb_next);
			//JIT: && !strncmp(ce->name->val, "__PHP_Incomplete_Class", ce->name->len)) {
			Value *str = zend_jit_load_str_val(llvm_ctx, name);
			zend_jit_expected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpNE(
					zend_jit_memcmp(llvm_ctx,
						str,
						LLVM_GET_CONST_STRING("__PHP_Incomplete_Class"),
						LLVM_GET_LONG(sizeof("__PHP_Incomplete_Class") - 1)),
					llvm_ctx.builder.getInt32(0)),
				bb_true,
				bb_false);
			break;
		}
		case IS_RESOURCE: {
			//JIT: if (Z_TYPE_P(value) == opline->extended_value) {
			bb_false = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_true = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			bb_next = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
			zend_jit_expected_br(llvm_ctx,
				llvm_ctx.builder.CreateICmpEQ(
					op1_type,
					llvm_ctx.builder.getInt8(IS_RESOURCE)),
				bb_next,
				bb_false);
			llvm_ctx.builder.SetInsertPoint(bb_next);
			//JIT: const char *type_name = zend_rsrc_list_get_rsrc_type(Z_RES_P(value) TSRMLS_CC);
			Value *res = zend_jit_load_res(llvm_ctx, op1_addr, OP1_SSA_VAR(), OP1_INFO());
			Value *name = zend_jit_rsrc_list_get_rsrc_type(llvm_ctx, res);
			//JIT: ZVAL_BOOL(EX_VAR(opline->result.var), type_name != NULL);
			zend_jit_unexpected_br(llvm_ctx,
					llvm_ctx.builder.CreateIsNull(name),
					bb_false,
					bb_true);
			break;
		}
		EMPTY_SWITCH_DEFAULT_CASE()
	}

	if (bb_true && bb_false) {
		BasicBlock *bb_common = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		
		llvm_ctx.builder.SetInsertPoint(bb_false);
		zend_jit_save_zval_type_info(llvm_ctx,
			res,
			RES_SSA_VAR(),
			RES_INFO(),
			llvm_ctx.builder.getInt32(IS_FALSE));
		llvm_ctx.builder.CreateBr(bb_common);

		llvm_ctx.builder.SetInsertPoint(bb_true);
		zend_jit_save_zval_type_info(llvm_ctx,
			res,
			RES_SSA_VAR(),
			RES_INFO(),
			llvm_ctx.builder.getInt32(IS_TRUE));
		llvm_ctx.builder.CreateBr(bb_common);

		llvm_ctx.builder.SetInsertPoint(bb_common);
	}

	//JIT: FREE_OP1();
	if (!zend_jit_free_operand(llvm_ctx, opline->op1_type, orig_op1_addr, NULL, OP1_SSA_VAR(), OP1_INFO(), opline->lineno)) return 0;

	if (OP1_MAY_BE(MAY_BE_OBJECT|MAY_BE_RESOURCE|MAY_BE_ARRAY_OF_OBJECT|MAY_BE_ARRAY_OF_RESOURCE)) {
		JIT_CHECK(zend_jit_check_exception(llvm_ctx, opline));
	}

	//JIT: ZEND_VM_NEXT_OPCODE();
	llvm_ctx.valid_opline = 0;
	return 1;
}
/* }}} */

/* {{{ static void zend_jit_assign_regs */
static int zend_jit_assign_regs(zend_llvm_ctx    &llvm_ctx,
                                zend_op_array    *op_array)
{	
	zend_jit_func_info *info = JIT_DATA(op_array);
	int i;
	Value *reg;
	Value **tmp_reg = (Value**)alloca(sizeof(Value*) * op_array->last_var * 4);

	memset(tmp_reg, 0, sizeof(Value*) * op_array->last_var * 4);
	for (i = 0; i < info->ssa.vars; i++) {
		if (info->ssa_var_info[i].type & MAY_BE_IN_REG) {
			if (info->ssa_var_info[i].type & (MAY_BE_NULL|MAY_BE_FALSE|MAY_BE_TRUE)) {
				if (info->ssa.var[i].var < op_array->last_var) {
					if (!tmp_reg[info->ssa.var[i].var * 4 + 0]) {
						tmp_reg[info->ssa.var[i].var * 4 + 0] = llvm_ctx.builder.CreateAlloca(Type::getInt32Ty(llvm_ctx.context));
					}
					reg = tmp_reg[info->ssa.var[i].var * 4 + 0];
				} else {
					reg = llvm_ctx.builder.CreateAlloca(Type::getInt32Ty(llvm_ctx.context));
				}
			} else if (info->ssa_var_info[i].type & MAY_BE_LONG) {
				if (info->ssa.var[i].var < op_array->last_var) {
					if (!tmp_reg[info->ssa.var[i].var * 4 + 1]) {
						tmp_reg[info->ssa.var[i].var * 4 + 1] = llvm_ctx.builder.CreateAlloca(Type::LLVM_GET_LONG_TY(llvm_ctx.context));
					}
					reg = tmp_reg[info->ssa.var[i].var * 4 + 1];
				} else {
					reg = llvm_ctx.builder.CreateAlloca(Type::LLVM_GET_LONG_TY(llvm_ctx.context));
				}
			} else if (info->ssa_var_info[i].type & MAY_BE_DOUBLE) {
				if (info->ssa.var[i].var < op_array->last_var) {
					if (!tmp_reg[info->ssa.var[i].var * 4 + 2]) {
						tmp_reg[info->ssa.var[i].var * 4 + 2] = llvm_ctx.builder.CreateAlloca(Type::getDoubleTy(llvm_ctx.context));
					}
					reg = tmp_reg[info->ssa.var[i].var * 4 + 2];
				} else {
					reg = llvm_ctx.builder.CreateAlloca(Type::getDoubleTy(llvm_ctx.context));
				}
			} else if (info->ssa_var_info[i].type & MAY_BE_STRING) {
				reg = llvm_ctx.builder.CreateAlloca(PointerType::getUnqual(llvm_ctx.zend_string_type));
			} else if (info->ssa_var_info[i].type & MAY_BE_ARRAY) {
				reg = llvm_ctx.builder.CreateAlloca(PointerType::getUnqual(llvm_ctx.zend_array_type));
			} else if (info->ssa_var_info[i].type & MAY_BE_OBJECT) {
				reg = llvm_ctx.builder.CreateAlloca(PointerType::getUnqual(llvm_ctx.zend_object_type));
			} else if (info->ssa_var_info[i].type & MAY_BE_RESOURCE) {
				reg = llvm_ctx.builder.CreateAlloca(PointerType::getUnqual(llvm_ctx.zend_res_type));
			}
			llvm_ctx.reg[i] = reg;
		}
	}
}
/* }}} */

/* {{{ static BasicBlock *zend_jit_ssa_target */
static BasicBlock *zend_jit_ssa_target(zend_llvm_ctx    &llvm_ctx,
                                       int               from_block,
                                       int               to_block)
{
	BasicBlock *target = llvm_ctx.bb_labels[to_block];
	zend_jit_func_info *info = JIT_DATA(llvm_ctx.op_array);

	if (info && info->ssa_var_info) {
		BasicBlock *bb_start = NULL;
		zend_jit_basic_block *b = info->cfg.block + to_block;
		zend_jit_ssa_phi *p = info->ssa.block[to_block].phis;
		int to_var, from_var;
		uint32_t to_info, from_info;
		BasicBlock *orig_bb;

		ZEND_ASSERT(from_block >= 0);
		for (; p; p = p->next) {
			to_var = p->ssa_var;
			from_var = -1;
			if (p->pi >= 0) {
				from_var = p->sources[0];
			} else {
				int j;

				for (j = 0; j < b->predecessors_count; j++) {
					if (info->cfg.predecessor[b->predecessor_offset + j] == from_block) {
						from_var = p->sources[j];
						break;
					}
				}
			}
			// TODO: ???
			if (from_var < 0) {
				continue;
			}
			ZEND_ASSERT(to_var >= 0 && from_var >= 0);
			if (info->ssa.var[from_var].no_val) {
				continue;
			}			
			to_info = info->ssa_var_info[to_var].type;
			from_info = info->ssa_var_info[from_var].type;
			if ((to_info & MAY_BE_IN_REG) || (from_info & MAY_BE_IN_REG)) {
				if (!bb_start) {
					orig_bb = llvm_ctx.builder.GetInsertBlock();
					bb_start = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
					llvm_ctx.builder.SetInsertPoint(bb_start);
				}
				Value *from_addr = NULL;
				Value *to_addr = NULL;

				if (!(from_info & MAY_BE_IN_REG)) {
					from_addr = zend_jit_load_slot(llvm_ctx, (zend_uintptr_t)ZEND_CALL_VAR_NUM(NULL, p->var));
				}

				if (!(to_info & MAY_BE_IN_REG)) {
					to_addr = zend_jit_load_slot(llvm_ctx, (zend_uintptr_t)ZEND_CALL_VAR_NUM(NULL, p->var));
				}

				if (to_info & MAY_BE_IN_REG) {
					if (to_info & (MAY_BE_NULL|MAY_BE_FALSE|MAY_BE_TRUE)) {
						zend_jit_save_zval_type_info(llvm_ctx, to_addr, to_var, to_info,
							zend_jit_load_type_info(llvm_ctx, from_addr, from_var, from_info));
					} else if (to_info & MAY_BE_LONG) {
						zend_jit_save_zval_lval(llvm_ctx, to_addr, to_var, to_info,
							zend_jit_load_lval(llvm_ctx, from_addr, from_var, from_info));
					} else if (to_info & MAY_BE_DOUBLE) {
						zend_jit_save_zval_dval(llvm_ctx, to_addr, to_var, to_info,
							zend_jit_load_dval(llvm_ctx, from_addr, from_var, from_info));
					} else {
						zend_jit_save_zval_ptr(llvm_ctx, to_addr, to_var, to_info,
							zend_jit_load_ptr(llvm_ctx, from_addr, from_var, from_info));
					}
				} else {
					zend_jit_save_zval_type_info(llvm_ctx, to_addr, to_var, to_info,
						zend_jit_load_type_info(llvm_ctx, from_addr, from_var, from_info));
					if (from_info & (MAY_BE_ANY-(MAY_BE_NULL|MAY_BE_FALSE|MAY_BE_TRUE))) {
						if (from_info & MAY_BE_LONG) {
							zend_jit_save_zval_lval(llvm_ctx, to_addr, to_var, to_info,
								zend_jit_load_lval(llvm_ctx, from_addr, from_var, from_info));
						} else if (from_info & MAY_BE_DOUBLE) {
							zend_jit_save_zval_dval(llvm_ctx, to_addr, to_var, to_info,
								zend_jit_load_dval(llvm_ctx, from_addr, from_var, from_info));
						} else {
							zend_jit_save_zval_ptr(llvm_ctx, to_addr, to_var, to_info,
								zend_jit_load_ptr(llvm_ctx, from_addr, from_var, from_info));
						}
					}
				}
			}
		}
		if (bb_start) {
			llvm_ctx.builder.CreateBr(target);
			llvm_ctx.builder.SetInsertPoint(orig_bb);
			return bb_start;
		}
	}

	return target;
}
/* }}} */

#define TARGET_BB(to_opline) \
	zend_jit_ssa_target(llvm_ctx, from_b, to_opline)

/* {{{ static int zend_jit_codegen_ex */
static int zend_jit_codegen_ex(zend_jit_context *ctx, 
                               zend_llvm_ctx    &llvm_ctx,
                               zend_op_array    *op_array)
{
	int i;
	int b, from_b;
	zend_jit_func_info *info = JIT_DATA(op_array);
	zend_jit_basic_block *block = info->cfg.block;
	zend_op *opline;

#if JIT_STAT
	if (info->flags & ZEND_JIT_FUNC_INLINE) {
		jit_stat.inlined_clones++;
	} else {
		if (info->clone_num) {
			jit_stat.compiled_clones++;
		} else {
			jit_stat.compiled_op_arrays++;
		}
	}

	jit_stat.ssa_vars += info->ssa.vars - op_array->last_var;
	if (info->ssa_var_info) {
		for (i = op_array->last_var; i < info->ssa.vars; i++) {
			if ((info->ssa_var_info[i].type & MAY_BE_ANY) == MAY_BE_ANY) {
				jit_stat.untyped_ssa_vars++;
			} else if (has_concrete_type(info->ssa_var_info[i].type)) {
				jit_stat.typed_ssa_vars++;
			}
			if (info->ssa_var_info[i].type & MAY_BE_IN_REG) {
				jit_stat.reg_ssa_vars++;
			}
		}
	} else {
		jit_stat.untyped_ssa_vars += info->ssa.vars - op_array->last_var;
	}
#endif
	
	llvm_ctx.op_array = op_array;
	llvm_ctx.reg = NULL;
	llvm_ctx.bb_labels = NULL;
	llvm_ctx.bb_exceptions = NULL;
	llvm_ctx.bb_exception_exit = NULL;
	llvm_ctx.bb_inline_return = NULL;
	llvm_ctx.bb_leave = NULL;
	llvm_ctx.call_level = 0;
	
	llvm_ctx.bb_labels = (BasicBlock**)zend_jit_context_calloc(ctx, sizeof(BasicBlock*), info->cfg.blocks);
	if (!llvm_ctx.bb_labels) return 0;

	if (op_array->last_try_catch) {
		llvm_ctx.bb_exceptions = (BasicBlock**)zend_jit_context_calloc(ctx, sizeof(BasicBlock*), op_array->last_try_catch);
		if (!llvm_ctx.bb_exceptions) return 0;
	}

	/* Find variables that may be allocated in registers */
	llvm_ctx.reg = (Value**)zend_jit_context_calloc(ctx, sizeof(Value*), info->ssa.vars);

//???	llvm_ctx.param_reg = (Value**)zend_jit_context_calloc(ctx, sizeof(Value*), op_array->used_stack);
//???	llvm_ctx.param_top = 0;

	// Create entry basic block
	BasicBlock *bb_start = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	if (llvm_ctx.inline_level) {
		llvm_ctx.builder.CreateBr(bb_start);		
	}
	llvm_ctx.builder.SetInsertPoint(bb_start);
//???	if (!llvm_ctx.inline_level && (info->flags & ZEND_JIT_FUNC_NO_FRAME)) {
//???		llvm_ctx._EX_Ts = zend_jit_allocate_tmp_vars(llvm_ctx, op_array);
//???	}
//???	if (op_array->used_stack) {
//???		AllocaInst *inst = llvm_ctx.builder.CreateAlloca(
//???				LLVM_GET_LONG_TY(llvm_ctx.context),
//???				LLVM_GET_LONG((sizeof(zval) * op_array->used_stack)/sizeof(long)));
//???		inst->setAlignment(4);
//???		llvm_ctx.param_tmp = inst;
//???	} else {
//???		llvm_ctx.param_tmp = NULL;
//???	}
	zend_jit_assign_regs(llvm_ctx, op_array);
//???	if (!zend_jit_preallocate_cvs(llvm_ctx, op_array)) return 0;

	if (block[0].flags & ZEND_BB_TARGET) {
		llvm_ctx.bb_labels[0] = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
	}
	for (b = 1; b < info->cfg.blocks; b++) {
		if (block[b].flags & ZEND_BB_REACHABLE) {
			llvm_ctx.bb_labels[b] = BasicBlock::Create(llvm_ctx.context, "", llvm_ctx.function);
		}
	}

	llvm_ctx.this_checked = (zend_bitset)zend_jit_context_calloc(ctx, sizeof(zend_ulong), zend_bitset_len(info->cfg.blocks));

	llvm_ctx.valid_opline = 1;

	from_b = -1;
	for (b = 0; b < info->cfg.blocks; b++) {
		if ((block[b].flags & ZEND_BB_REACHABLE) == 0) {
			continue;
		}
		if (b > 0 || (block[b].flags & ZEND_BB_TARGET)) {
			BasicBlock *bb = llvm_ctx.builder.GetInsertBlock();
			if (bb && !bb->getTerminator()) {			
				llvm_ctx.builder.CreateBr(TARGET_BB(b));
			}
			llvm_ctx.builder.SetInsertPoint(llvm_ctx.bb_labels[b]);
		}
		if (block[b].flags & ZEND_BB_TARGET) {
			llvm_ctx.valid_opline = 0;
		} else if (b > 0 && block[b - 1].end + 1 != block[b].start) {
			llvm_ctx.valid_opline = 0;
		}

		from_b = b;
		for (i = block[b].start; i <= block[b].end; i++) {
			opline = op_array->opcodes + i;

//			if (supports_reg_alloc(opline)) {
//				zend_jit_allocate_operands(ctx, opline, &allocation);
//			}

			switch (opline->opcode) {
				case ZEND_NOP:
					llvm_ctx.valid_opline = 0;
					break;
				case ZEND_OP_DATA:
					llvm_ctx.valid_opline = 0;
					break;
				case ZEND_JMP:
					llvm_ctx.builder.CreateBr(TARGET_BB(block[b].successors[0]));
					llvm_ctx.valid_opline = 0;
					break;
				case ZEND_JMPZ:
					if (!zend_jit_jmpznz(
							llvm_ctx,
							op_array,
							opline,
							TARGET_BB(block[b].successors[0]),
							TARGET_BB(block[b].successors[1]),
							1)) return 0;
					llvm_ctx.valid_opline = 0;
					break;
				case ZEND_JMPNZ:
					if (!zend_jit_jmpznz(
							llvm_ctx,
							op_array,
							opline,
							TARGET_BB(block[b].successors[1]),
							TARGET_BB(block[b].successors[0]),
							0)) return 0;
					llvm_ctx.valid_opline = 0;
					break;
				case ZEND_JMPZNZ:
					if (!zend_jit_jmpznz(
							llvm_ctx,
							op_array,
							opline,
							TARGET_BB(block[b].successors[1]),
							TARGET_BB(block[b].successors[0]),
							-1)) return 0;
					llvm_ctx.valid_opline = 0;
					break;
				case ZEND_ADD:
				case ZEND_SUB:
				case ZEND_MUL:
				case ZEND_DIV:
					zend_jit_math_op(llvm_ctx, op_array, opline);
					llvm_ctx.valid_opline = 0;
					break;
				case ZEND_MOD:
				case ZEND_SL:
				case ZEND_SR:
				case ZEND_BW_OR:
				case ZEND_BW_AND:
				case ZEND_BW_XOR:
					zend_jit_long_math_op(llvm_ctx, op_array, opline);
					llvm_ctx.valid_opline = 0;
					break;
				case ZEND_BW_NOT:
					zend_jit_bw_not(llvm_ctx, op_array, opline);
					llvm_ctx.valid_opline = 0;
					break;
				case ZEND_BOOL_NOT:
					zend_jit_bool(llvm_ctx, op_array, opline, 1);
					llvm_ctx.valid_opline = 0;
					break;
				case ZEND_BOOL:
					zend_jit_bool(llvm_ctx, op_array, opline, 0);
					llvm_ctx.valid_opline = 0;
					break;
#if 0 // TODO: reimplement FAST_CONCATR and ROPE_* ???
				case ZEND_ADD_CHAR:
					if (!zend_jit_add_string(llvm_ctx, op_array, opline)) return 0;
					break;
				case ZEND_ADD_STRING:
					if (!zend_jit_add_string(llvm_ctx, op_array, opline)) return 0;
					break;
				case ZEND_ADD_VAR:
					if (!zend_jit_add_string(llvm_ctx, op_array, opline)) return 0;
					break;
#endif
				case ZEND_CONCAT:
					if (!zend_jit_concat(llvm_ctx, ctx, op_array, opline)) return 0;
					break;
				case ZEND_IS_IDENTICAL:
				case ZEND_IS_NOT_IDENTICAL:
				case ZEND_IS_EQUAL:
				case ZEND_IS_NOT_EQUAL:
				case ZEND_IS_SMALLER:
				case ZEND_IS_SMALLER_OR_EQUAL:
				case ZEND_CASE:
					if (i != block[b].end &&
					    (opline+1)->opcode == ZEND_JMPZ &&
					    (opline+1)->op1_type == IS_TMP_VAR &&
					    (opline+1)->op1.var == RES_OP()->var) {
						if (!zend_jit_cmp(llvm_ctx, op_array, opline,
							TARGET_BB(block[b].successors[0]),
							TARGET_BB(block[b].successors[1]),
							1)) return 0;
						i++;
					} else if (i != block[b].end &&
					    (opline+1)->opcode == ZEND_JMPNZ &&
					    (opline+1)->op1_type == IS_TMP_VAR &&
					    (opline+1)->op1.var == RES_OP()->var) {
						if (!zend_jit_cmp(llvm_ctx, op_array, opline,
							TARGET_BB(block[b].successors[1]),
							TARGET_BB(block[b].successors[0]),
							0)) return 0;
						i++;
					} else if (i != block[b].end &&
					    (opline+1)->opcode == ZEND_JMPZNZ &&
					    (opline+1)->op1_type == IS_TMP_VAR &&
					    (opline+1)->op1.var == RES_OP()->var) {
						if (!zend_jit_cmp(llvm_ctx, op_array, opline,
							TARGET_BB(block[b].successors[1]),
							TARGET_BB(block[b].successors[0]),
							-1)) return 0;
						i++;
					} else {
						if (!zend_jit_cmp(llvm_ctx, op_array, opline, NULL, NULL, -1)) return 0;
					}
					llvm_ctx.valid_opline = 0;
					break;
				case ZEND_QM_ASSIGN:
					if (!zend_jit_qm_assign(llvm_ctx, op_array, opline)) return 0;
					break;
				case ZEND_ASSIGN:
					if (!zend_jit_assign(llvm_ctx, op_array, opline)) return 0;
					break;
				case ZEND_ASSIGN_DIM:
					if (!zend_jit_assign_dim(llvm_ctx, ctx, op_array, opline)) return 0;
					break;
				case ZEND_ASSIGN_OBJ:
					if (!zend_jit_assign_obj(llvm_ctx, ctx, op_array, b, opline)) return 0;
					break;
				case ZEND_PRE_INC:
				case ZEND_PRE_DEC:
				case ZEND_POST_INC:
				case ZEND_POST_DEC:
					if (!zend_jit_incdec(llvm_ctx, op_array, opline)) return 0;
					break;
				case ZEND_RECV:
				case ZEND_RECV_INIT:
					if (!zend_jit_recv(llvm_ctx, op_array, opline)) return 0;
					llvm_ctx.valid_opline = 0;
					break;
//???
#if 0
				case ZEND_FETCH_OBJ_R:
					if (!zend_jit_fetch_obj_r(llvm_ctx, ctx, op_array, opline)) return 0;
					break;
				case ZEND_FETCH_OBJ_W:
					if (!zend_jit_fetch_obj(llvm_ctx, ctx, op_array, opline, BP_VAR_W)) return 0;
					break;
				case ZEND_FETCH_OBJ_RW:
					if (!zend_jit_fetch_obj(llvm_ctx, ctx, op_array, opline, BP_VAR_RW)) return 0;
					break;
				case ZEND_FETCH_CONSTANT:
					if (!zend_jit_fetch_const(llvm_ctx, op_array, opline)) return 0;
					break;
#endif
				case ZEND_FETCH_DIM_W:
					if (!zend_jit_fetch_dim(llvm_ctx, ctx, op_array, opline, BP_VAR_W)) return 0;
					break;
				case ZEND_FETCH_DIM_R:
					if (!zend_jit_fetch_dim_r(llvm_ctx, ctx, op_array, opline, BP_VAR_R)) return 0;
					break;
				case ZEND_FETCH_DIM_IS:
					if (!zend_jit_fetch_dim_r(llvm_ctx, ctx, op_array, opline, BP_VAR_IS)) return 0;
					break;
				case ZEND_FETCH_DIM_RW:
					if (!zend_jit_fetch_dim(llvm_ctx, ctx, op_array, opline, BP_VAR_RW)) return 0;
					break;
				case ZEND_ASSIGN_ADD:
					if (!zend_jit_assign_op(llvm_ctx, ctx, op_array, opline, ZEND_ADD)) return 0;
					break;
				case ZEND_ASSIGN_SUB:
					if (!zend_jit_assign_op(llvm_ctx, ctx, op_array, opline, ZEND_SUB)) return 0;
					break;
				case ZEND_ASSIGN_MUL:
					if (!zend_jit_assign_op(llvm_ctx, ctx, op_array, opline, ZEND_MUL)) return 0;
					break;
				case ZEND_ASSIGN_DIV:
					if (!zend_jit_assign_op(llvm_ctx, ctx, op_array, opline, ZEND_DIV)) return 0;
					break;
				case ZEND_ASSIGN_CONCAT:
					if (!zend_jit_assign_op(llvm_ctx, ctx, op_array, opline, ZEND_CONCAT)) return 0;
					break;
#if 0
				case ZEND_ASSIGN_MOD:
					if (!zend_jit_assign_op(llvm_ctx, ctx, op_array, opline, ZEND_MOD)) return 0;
					break;
				case ZEND_ASSIGN_SL:
					if (!zend_jit_assign_op(llvm_ctx, ctx, op_array, opline, ZEND_SL)) return 0;
					break;
				case ZEND_ASSIGN_SR:
					if (!zend_jit_assign_op(llvm_ctx, ctx, op_array, opline, ZEND_SR)) return 0;
					break;
				case ZEND_ASSIGN_BW_OR:
					if (!zend_jit_assign_op(llvm_ctx, ctx, op_array, opline, ZEND_BW_OR)) return 0;
					break;
				case ZEND_ASSIGN_BW_AND:
					if (!zend_jit_assign_op(llvm_ctx, ctx, op_array, opline, ZEND_BW_AND)) return 0;
					break;
				case ZEND_ASSIGN_BW_XOR:
					if (!zend_jit_assign_op(llvm_ctx, ctx, op_array, opline, ZEND_BW_XOR)) return 0;
					break;
#endif
				case ZEND_RETURN:
					if (!zend_jit_return(llvm_ctx, op_array, opline)) return 0;
					break;
				case ZEND_DO_FCALL:
				case ZEND_DO_ICALL:
				case ZEND_DO_UCALL:
				case ZEND_DO_FCALL_BY_NAME:
					if (!zend_jit_do_fcall(llvm_ctx, ctx, op_array, opline)) return 0;
					llvm_ctx.call_level--;
					break;
				case ZEND_INIT_FCALL:
//				case ZEND_INIT_FCALL_BY_NAME:
					llvm_ctx.call_level++;
					if (!zend_jit_init_fcall(llvm_ctx, ctx, op_array, opline)) return 0;
					break;
				case ZEND_SEND_VAL:
					if (!zend_jit_send_val(llvm_ctx, op_array, opline, 0)) return 0;
					llvm_ctx.valid_opline = 0;
					break;
				case ZEND_SEND_VAL_EX:
					if (!zend_jit_send_val(llvm_ctx, op_array, opline, 1)) return 0;
					llvm_ctx.valid_opline = 0;
					break;
				case ZEND_SEND_REF:
					if (!zend_jit_send_ref(llvm_ctx, op_array, opline)) return 0;
					break;
				case ZEND_SEND_VAR:
					if (!zend_jit_send_var(llvm_ctx, op_array, opline, 0)) return 0;
					break;
				case ZEND_SEND_VAR_EX:
					if (!zend_jit_send_var(llvm_ctx, op_array, opline, 1)) return 0;
					break;
				case ZEND_SEND_VAR_NO_REF:
					if (!zend_jit_send_var_no_ref(llvm_ctx, op_array, opline)) return 0;
					break;
				case ZEND_ECHO:
					if (!zend_jit_echo(llvm_ctx, op_array, opline)) return 0;
					break;
				case ZEND_FREE:
					if (!zend_jit_free(llvm_ctx, op_array, opline)) return 0;
					break;
				case ZEND_TYPE_CHECK:
					if (!zend_jit_type_check(llvm_ctx, op_array, opline)) return 0;
					break;
//???
#if 0
				case ZEND_INIT_ARRAY:
				case ZEND_ADD_ARRAY_ELEMENT:
					if (!zend_jit_add_array_element(llvm_ctx, op_array, opline)) return 0;
					break;
#endif
				case ZEND_ISSET_ISEMPTY_DIM_OBJ:
					if (!zend_jit_isset_isempty_dim_obj(llvm_ctx, ctx, op_array, b, opline)) return 0;
					break;
				/* Support for ZEND_VM_SMART_BRANCH */
				case ZEND_ISSET_ISEMPTY_VAR:
				case ZEND_ISSET_ISEMPTY_PROP_OBJ:
				case ZEND_ISSET_ISEMPTY_STATIC_PROP:
				case ZEND_INSTANCEOF:
				case ZEND_DEFINED:
					if (!zend_jit_handler(llvm_ctx, opline)) return 0;
					if ((opline+1)->opcode == ZEND_JMPZ || (opline+1)->opcode == ZEND_JMPNZ) {
						opline++;
						i++;
						if (!zend_jit_cond_jmp(llvm_ctx, opline, OP_JMP_ADDR(opline, *OP2_OP()), TARGET_BB(block[b].successors[0]), TARGET_BB(block[b].successors[1]))) return 0;
					}
					break;
#if 0
				case ZEND_FE_FETCH_R:
				case ZEND_FE_FETCH_RW:
					if (!zend_jit_fe_fetch(llvm_ctx, op_array, ctx, opline)) return 0;
					break;
//				case ZEND_FAST_CALL:
//					if (!zend_jit_fast_call(ctx, asm_buf, opline, labels)) return 0;
//					break;
//				case ZEND_FAST_RET:
//					if (!zend_jit_fast_ret(ctx, asm_buf, opline, labels)) return 0;
//					break;
#endif
				case ZEND_GENERATOR_RETURN:
				case ZEND_RETURN_BY_REF:
				case ZEND_EXIT:
					if (!zend_jit_tail_handler(llvm_ctx, opline)) return 0;
					break;
//				case ZEND_DO_FCALL:
//				case ZEND_DO_ICALL:
//				case ZEND_DO_UCALL:
//				case ZEND_DO_FCALL_BY_NAME:
//					if (zend_hash_find(EG(function_table), Z_STRVAL_P(OP1_OP()->zv), Z_STRLEN_P(OP1_OP()->zv)+1, (void**)&fn) == SUCCESS &&
//					    fn->type == ZEND_INTERNAL_FUNCTION) {
//						if (!zend_jit_handler(asm_buf, opline)) return 0;
//						break;
//					}
//				case ZEND_DO_FCALL_BY_NAME:
//				case ZEND_INCLUDE_OR_EVAL:
//				case ZEND_YIELD:
//				case ZEND_YIELD_FROM:
//					if (!zend_jit_tail_handler(asm_buf, opline)) return 0;
//					ASSERT(IS_ENTRY_BLOCK(i+1));
//					break;
				case ZEND_NEW:
					llvm_ctx.call_level++;
					/* break missing intentionally */
				case ZEND_JMPZ_EX:
				case ZEND_JMPNZ_EX:
				case ZEND_JMP_SET:
				case ZEND_COALESCE:
				case ZEND_FE_RESET_R:
				case ZEND_FE_RESET_RW:
				case ZEND_ASSERT_CHECK:
					if (!zend_jit_handler(llvm_ctx, opline)) return 0;
					if (!zend_jit_cond_jmp(llvm_ctx, opline, OP_JMP_ADDR(opline, *OP2_OP()), TARGET_BB(block[b].successors[0]), TARGET_BB(block[b].successors[1]))) return 0;
					break;
				case ZEND_FE_FETCH_R:
				case ZEND_FE_FETCH_RW:
					if (!zend_jit_handler(llvm_ctx, opline)) return 0;
					if (!zend_jit_cond_jmp(llvm_ctx, opline, ZEND_OFFSET_TO_OPLINE(opline, opline->extended_value), TARGET_BB(block[b].successors[0]), TARGET_BB(block[b].successors[1]))) return 0;
					break;
				case ZEND_THROW:
					if (!zend_jit_store_opline(llvm_ctx, opline)) return 0;
					if (!zend_jit_call_handler(llvm_ctx, opline, 0)) return 0;
					if (!zend_jit_throw_exception(llvm_ctx, opline)) return 0;
					break;
				case ZEND_CATCH:
					if (!zend_jit_store_opline(llvm_ctx, opline)) return 0;
					if (!zend_jit_call_handler(llvm_ctx, opline, 0)) return 0;
					if (opline->result.num) {
						if (!zend_jit_check_exception(llvm_ctx, opline)) return 0;
					}
					if (!zend_jit_cond_jmp(llvm_ctx, opline, ZEND_OFFSET_TO_OPLINE(opline, opline->extended_value), TARGET_BB(block[b].successors[0]), TARGET_BB(block[b].successors[1]))) return 0;
					break;
				case ZEND_DECLARE_ANON_CLASS:
				case ZEND_DECLARE_ANON_INHERITED_CLASS:
					if (!zend_jit_handler(llvm_ctx, opline)) return 0;
					if (!zend_jit_cond_jmp(llvm_ctx, opline, OP_JMP_ADDR(opline, *OP1_OP()), TARGET_BB(block[b].successors[0]), TARGET_BB(block[b].successors[1]))) return 0;
					break;
				case ZEND_BIND_GLOBAL:
					if (!zend_jit_bind_global(llvm_ctx, op_array, opline)) return 0;
					break;
				case ZEND_STRLEN:
					if (!zend_jit_strlen(llvm_ctx, op_array, opline)) return 0;
					break;
				case ZEND_INIT_FCALL_BY_NAME:
				case ZEND_INIT_NS_FCALL_BY_NAME:
				case ZEND_INIT_METHOD_CALL:
				case ZEND_INIT_DYNAMIC_CALL:
				case ZEND_INIT_STATIC_METHOD_CALL:
				case ZEND_INIT_USER_CALL:
					llvm_ctx.call_level++;
					/* break missing intentionally */
				default:
					if (!zend_jit_handler(llvm_ctx, opline)) return 0;
					break;
			}
		}

		/* Insert implicit JMP, introduced by block sorter, if necessary */
		if (block[b].successors[0] >= 0 &&
		    block[b].successors[1] < 0 &&
//??? "i" or "b"?
		    block[b].successors[0] != i + 1) {
			switch (op_array->opcodes[block[b].end].opcode) {
				case ZEND_JMP:
					break;
				default:
					llvm_ctx.builder.CreateBr(TARGET_BB(block[b].successors[0]));
					break;
			}
		}
	}

	if (!llvm_ctx.builder.GetInsertBlock()->getTerminator()) {
		if (llvm_ctx.inline_level) {
			if (llvm_ctx.bb_inline_return) {
				llvm_ctx.builder.CreateBr(llvm_ctx.bb_inline_return);
			}
		} else {
			if (info->clone_num) {
				if (info->return_info.type & MAY_BE_IN_REG) {
					if (info->return_info.type & MAY_BE_DOUBLE) {
						llvm_ctx.builder.CreateRet(ConstantFP::get(Type::getDoubleTy(llvm_ctx.context), 0.0));
					} else if (info->return_info.type & (MAY_BE_LONG|MAY_BE_FALSE|MAY_BE_TRUE)) {
						llvm_ctx.builder.CreateRet(LLVM_GET_LONG(0));
					} else {
						ASSERT_NOT_REACHED();
					}
				} else {
					llvm_ctx.builder.CreateRetVoid();
				}
			} else {
				llvm_ctx.builder.CreateRet(llvm_ctx.builder.getInt32(-1));
			}
		}
	}
	if (llvm_ctx.inline_level) {
		if (llvm_ctx.bb_inline_return) {
			llvm_ctx.builder.SetInsertPoint(llvm_ctx.bb_inline_return);
		}
	}

	return 1;
}
/* }}} */

/* {{{ static int zend_jit_codegen_start_module */
static int zend_jit_codegen_start_module(zend_jit_context *ctx, zend_op_array *op_array TSRMLS_DC)
{
	zend_llvm_ctx *ptr = new zend_llvm_ctx(getGlobalContext());
	zend_llvm_ctx &llvm_ctx = *ptr;
	ctx->codegen_ctx = (void*)ptr;

	llvm_ctx.module  = new Module("jit", llvm_ctx.context);

	std::string ErrorMsg;
    EngineBuilder TheBuilder(llvm_ctx.module);

	TheBuilder.setErrorStr(&ErrorMsg);
	TheBuilder.setEngineKind(EngineKind::JIT);
	if ((ZCG(accel_directives).jit_opt & JIT_OPT_CODEGEN) == JIT_OPT_CODEGEN_O3) {
		TheBuilder.setOptLevel(CodeGenOpt::Aggressive);
	} else if ((ZCG(accel_directives).jit_opt & JIT_OPT_CODEGEN) == JIT_OPT_CODEGEN_O2) {
		TheBuilder.setOptLevel(CodeGenOpt::Default);
	} else if ((ZCG(accel_directives).jit_opt & JIT_OPT_CODEGEN) == JIT_OPT_CODEGEN_O1) {
		TheBuilder.setOptLevel(CodeGenOpt::Less);
	} else {
		TheBuilder.setOptLevel(CodeGenOpt::None);
	}
#if ZEND_LLVM_DEBUG & ZEND_LLVM_DEBUG_GDB
	TheBuilder.setUseMCJIT(true);
	ZendJITMemoryManager *JMM = new ZendJITMemoryManager();
	TheBuilder.setJITMemoryManager(JMM);
//	TheBuilder.setJITMemoryManager(JITMemoryManager::CreateDefaultMemManager());
#endif

#if ZEND_LLVM_DEBUG & (ZEND_LLVM_DEBUG_CODEGEN | ZEND_LLVM_DEBUG_GDB)
    TargetOptions TheOptions;
#if ZEND_LLVM_DEBUG & ZEND_LLVM_DEBUG_CODEGEN
    TheOptions.PrintMachineCode = 1;
#endif
#if ZEND_LLVM_DEBUG & ZEND_LLVM_DEBUG_GDB
    TheOptions.JITEmitDebugInfo = 1;
#endif
    TheBuilder.setTargetOptions(TheOptions);
#endif
	
#if defined(__x86_64__)
	TheBuilder.setMArch("x86-64");
#else
	TheBuilder.setMArch("x86");
#endif	
#if (LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR >= 5)
//	TheBuilder.setMAttrs(MAttrs);
	TheBuilder.setMCPU(llvm::sys::getHostCPUName());
#endif

	llvm_ctx.target = TheBuilder.selectTarget();
	llvm_ctx.engine = TheBuilder.create();

#if ZEND_LLVM_DEBUG & ZEND_LLVM_DEBUG_GDB
	JMM->setEngine(llvm_ctx.engine, llvm_ctx.module);
#endif

#ifdef HAVE_OPROFILE
	if (event_listener) {
		llvm_ctx.engine->RegisterJITEventListener(event_listener);
	}
#endif

	llvm_ctx.zval_type = ArrayType::get(LLVM_GET_LONG_TY(llvm_ctx.context), sizeof(zval) / sizeof(long));
	llvm_ctx.zval_ptr_type = PointerType::getUnqual(llvm_ctx.zval_type);
	llvm_ctx.zend_string_type = ArrayType::get(LLVM_GET_LONG_TY(llvm_ctx.context), sizeof(zend_string) / sizeof(long));
	llvm_ctx.zend_res_type = ArrayType::get(LLVM_GET_LONG_TY(llvm_ctx.context), sizeof(zend_resource) / sizeof(long));
	llvm_ctx.zend_array_type = ArrayType::get(LLVM_GET_LONG_TY(llvm_ctx.context), sizeof(zend_array) / sizeof(long));
	llvm_ctx.HashTable_type = ArrayType::get(LLVM_GET_LONG_TY(llvm_ctx.context), sizeof(HashTable) / sizeof(long));
	llvm_ctx.zend_execute_data_type = ArrayType::get(LLVM_GET_LONG_TY(llvm_ctx.context), sizeof(zend_execute_data) / sizeof(long));
	llvm_ctx.zend_vm_stack_type = ArrayType::get(LLVM_GET_LONG_TY(llvm_ctx.context), sizeof(struct _zend_vm_stack) / sizeof(long));
	llvm_ctx.zend_constant_type = ArrayType::get(LLVM_GET_LONG_TY(llvm_ctx.context), sizeof(zend_constant) / sizeof(long));
	llvm_ctx.zend_function_type = ArrayType::get(LLVM_GET_LONG_TY(llvm_ctx.context), sizeof(zend_function) / sizeof(long));
	llvm_ctx.zend_op_array_type = ArrayType::get(LLVM_GET_LONG_TY(llvm_ctx.context), sizeof(zend_op_array) / sizeof(long));
	llvm_ctx.zend_class_entry_type = ArrayType::get(LLVM_GET_LONG_TY(llvm_ctx.context), sizeof(zend_class_entry) / sizeof(long));
	llvm_ctx.zend_op_type = ArrayType::get(LLVM_GET_LONG_TY(llvm_ctx.context), sizeof(zend_op) / sizeof(long));
	llvm_ctx.zend_refcounted_type = ArrayType::get(LLVM_GET_LONG_TY(llvm_ctx.context), sizeof(zend_refcounted) / sizeof(long));
	llvm_ctx.zend_object_type = ArrayType::get(LLVM_GET_LONG_TY(llvm_ctx.context), sizeof(zend_object) / sizeof(long));
	llvm_ctx.zend_property_info_type = ArrayType::get(LLVM_GET_LONG_TY(llvm_ctx.context), sizeof(zend_property_info) / sizeof(long));
	llvm_ctx.zend_arena_type = ArrayType::get(LLVM_GET_LONG_TY(llvm_ctx.context), sizeof(zend_arena) / sizeof(long));

	llvm_ctx.handler_type = FunctionType::get(
		Type::getInt32Ty(llvm_ctx.context),
		ArrayRef<Type*>(PointerType::getUnqual(llvm_ctx.zend_execute_data_type)),
		false);

	std::vector<llvm::Type *> internal_func_args;
	internal_func_args.push_back(PointerType::getUnqual(llvm_ctx.zend_execute_data_type));
	internal_func_args.push_back(llvm_ctx.zval_ptr_type);
	llvm_ctx.internal_func_type = FunctionType::get(
		Type::getVoidTy(llvm_ctx.context),
		ArrayRef<Type*>(internal_func_args),
		false);

	if (is_zend_mm(TSRMLS_C)) {
		llvm_ctx.mm_heap = zend_mm_set_heap(NULL TSRMLS_DC);
		zend_mm_set_heap(llvm_ctx.mm_heap TSRMLS_DC);

		llvm_ctx.mm_alloc = (void*)_zend_mm_alloc;
		llvm_ctx.mm_free = (void*)_zend_mm_free;

#if defined(__i386__) || defined(__x86_64__)
		/* A hack to call _zend_mm_alloc_int/_zend_mm_free_int directly */
		unsigned char *p;

		p = (unsigned char*)_zend_mm_alloc;
		if (*p == 0xe9) { /* jmp _zend_mm_alloc_int */
			llvm_ctx.mm_alloc = (void*)(p + *(int*)(p+1) + 1 + sizeof(int));
		}

		p = (unsigned char*)_zend_mm_free;
		if (*p == 0xe9) { /* jmp _zend_mm_free_int */
			llvm_ctx.mm_free = (void*)(p + *(int*)(p+1) + 1 + sizeof(int));
		}
#endif
	}

	// Create LLVM reference to CG(empty_string)
	llvm_ctx._CG_empty_string = new GlobalVariable(
			*llvm_ctx.module,
			llvm_ctx.zend_string_type,
			false,
			GlobalVariable::ExternalLinkage,
			0,
			ZEND_JIT_SYM("CG_emptry_string"));
	llvm_ctx.engine->addGlobalMapping(llvm_ctx._CG_empty_string, (void*)CG(empty_string));

	// Create LLVM reference to CG(one_char_string)
	llvm_ctx._CG_one_char_string = new GlobalVariable(
			*llvm_ctx.module,
			PointerType::getUnqual(llvm_ctx.zend_string_type),
			false,
			GlobalVariable::ExternalLinkage,
			0,
			ZEND_JIT_SYM("CG_one_char_string"));
	llvm_ctx.engine->addGlobalMapping(llvm_ctx._CG_one_char_string, (void*)CG(one_char_string));

	// Create LLVM reference to CG(arena)
	llvm_ctx._CG_arena = new GlobalVariable(
			*llvm_ctx.module,
			PointerType::getUnqual(llvm_ctx.zend_arena_type),
			false,
			GlobalVariable::ExternalLinkage,
			0,
			ZEND_JIT_SYM("CG_arena"));
	llvm_ctx.engine->addGlobalMapping(llvm_ctx._CG_arena, (void*)&CG(arena));

	// Create LLVM reference to EG(exception)
	llvm_ctx._EG_exception = new GlobalVariable(
			*llvm_ctx.module,
			llvm_ctx.zval_ptr_type,
			false,
			GlobalVariable::ExternalLinkage,
			0,
			ZEND_JIT_SYM("EG_exception"));
	llvm_ctx.engine->addGlobalMapping(llvm_ctx._EG_exception, (void*)&EG(exception));

	// Create LLVM reference to EG(vm_stack_top)
	llvm_ctx._EG_vm_stack_top = new GlobalVariable(
			*llvm_ctx.module,
			LLVM_GET_LONG_TY(llvm_ctx.context),
			false,
			GlobalVariable::ExternalLinkage,
			0,
			ZEND_JIT_SYM("EG_vm_stack_top"));
	llvm_ctx.engine->addGlobalMapping(llvm_ctx._EG_vm_stack_top, (void*)&EG(vm_stack_top));
	
	// Create LLVM reference to EG(vm_stack_end)
	llvm_ctx._EG_vm_stack_end = new GlobalVariable(
			*llvm_ctx.module,
			LLVM_GET_LONG_TY(llvm_ctx.context),
			false,
			GlobalVariable::ExternalLinkage,
			0,
			ZEND_JIT_SYM("EG_vm_stack_end"));
	llvm_ctx.engine->addGlobalMapping(llvm_ctx._EG_vm_stack_end, (void*)&EG(vm_stack_end));
	
	// Create LLVM reference to EG(vm_stack)
	llvm_ctx._EG_vm_stack = new GlobalVariable(
			*llvm_ctx.module,
			PointerType::getUnqual(llvm_ctx.zend_vm_stack_type),
			false,
			GlobalVariable::ExternalLinkage,
			0,
			ZEND_JIT_SYM("EG_vm_stack"));
	llvm_ctx.engine->addGlobalMapping(llvm_ctx._EG_vm_stack, (void*)&EG(vm_stack));
	
	// Create LLVM reference to EG(objects_store)
	llvm_ctx._EG_objects_store = new GlobalVariable(
			*llvm_ctx.module,
			PointerType::getUnqual(LLVM_GET_LONG_TY(llvm_ctx.context)),
			false,
			GlobalVariable::ExternalLinkage,
			0,
			ZEND_JIT_SYM("EG_objects_store"));
	llvm_ctx.engine->addGlobalMapping(llvm_ctx._EG_objects_store, (void*)&EG(objects_store));

	// Create LLVM reference to EG(uninitialized_zval)
	llvm_ctx._EG_uninitialized_zval = new GlobalVariable(
			*llvm_ctx.module,
			llvm_ctx.zval_type,
			false,
			GlobalVariable::ExternalLinkage,
			0,
			ZEND_JIT_SYM("EG_uninitialized_zval"));
	llvm_ctx.engine->addGlobalMapping(llvm_ctx._EG_uninitialized_zval, (void*)&EG(uninitialized_zval));

	// Create LLVM reference to EG(error_zval)
	llvm_ctx._EG_error_zval = new GlobalVariable(
			*llvm_ctx.module,
			llvm_ctx.zval_type,
			false,
			GlobalVariable::ExternalLinkage,
			0,
			ZEND_JIT_SYM("EG_error_zval"));
	llvm_ctx.engine->addGlobalMapping(llvm_ctx._EG_error_zval, (void*)&EG(error_zval));

	// Create LLVM reference to EG(current_execute_data)
	llvm_ctx._EG_current_execute_data = new GlobalVariable(
			*llvm_ctx.module,
			PointerType::getUnqual(llvm_ctx.zend_execute_data_type),
			false,
			GlobalVariable::ExternalLinkage,
			0,
			ZEND_JIT_SYM("EG_current_execute_data"));
	llvm_ctx.engine->addGlobalMapping(llvm_ctx._EG_current_execute_data, (void*)&EG(current_execute_data));

	// Create LLVM reference to EG(function_table)
	llvm_ctx._EG_function_table = new GlobalVariable(
			*llvm_ctx.module,
			PointerType::getUnqual(llvm_ctx.HashTable_type),
			false,
			GlobalVariable::ExternalLinkage,
			0,
			ZEND_JIT_SYM("EG_function_table"));
	llvm_ctx.engine->addGlobalMapping(llvm_ctx._EG_function_table, (void*)&EG(function_table));

	// Create LLVM reference to EG(scope)
	llvm_ctx._EG_scope = new GlobalVariable(
			*llvm_ctx.module,
			PointerType::getUnqual(llvm_ctx.zend_class_entry_type),
			false,
			GlobalVariable::ExternalLinkage,
			0,
			ZEND_JIT_SYM("EG_scope"));
	llvm_ctx.engine->addGlobalMapping(llvm_ctx._EG_scope, (void*)&EG(scope));

	// Create LLVM reference to EG(symbol_table))
	llvm_ctx._EG_symbol_table = new GlobalVariable(
			*llvm_ctx.module,
			PointerType::getUnqual(llvm_ctx.zend_array_type),
			false,
			GlobalVariable::ExternalLinkage,
			0,
			ZEND_JIT_SYM("EG_symbol_table"));
	llvm_ctx.engine->addGlobalMapping(llvm_ctx._EG_symbol_table, (void*)&EG(symbol_table));

	// Create LLVM reference to EG(symtable_cache_ptr)
	llvm_ctx._EG_symtable_cache_ptr = new GlobalVariable(
			*llvm_ctx.module,
			PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.HashTable_type)),
			false,
			GlobalVariable::ExternalLinkage,
			0,
			ZEND_JIT_SYM("EG_symtable_cache_ptr"));
	llvm_ctx.engine->addGlobalMapping(llvm_ctx._EG_symtable_cache_ptr, (void*)&EG(symtable_cache_ptr));

	// Create LLVM reference to EG(symtable_cache_limit)
	llvm_ctx._EG_symtable_cache_limit = new GlobalVariable(
			*llvm_ctx.module,
			PointerType::getUnqual(PointerType::getUnqual(llvm_ctx.HashTable_type)),
			false,
			GlobalVariable::ExternalLinkage,
			0,
			ZEND_JIT_SYM("EG_symtable_cache_limit"));
	llvm_ctx.engine->addGlobalMapping(llvm_ctx._EG_symtable_cache_limit, (void*)&EG(symtable_cache_limit));

	// Create LLVM reference to EG(precision)
	llvm_ctx._EG_precision = new GlobalVariable(
			*llvm_ctx.module,
			LLVM_GET_LONG_TY(llvm_ctx.context),
			false,
			GlobalVariable::ExternalLinkage,
			0,
			ZEND_JIT_SYM("EG_precision"));
	llvm_ctx.engine->addGlobalMapping(llvm_ctx._EG_precision, (void*)&EG(precision));

	// Create LLVM reference to zend_execute_internal
	std::vector<llvm::Type *> args;
	args.push_back(PointerType::getUnqual(llvm_ctx.zend_execute_data_type));
	args.push_back(LLVM_GET_LONG_TY(llvm_ctx.context)); // FIXME: change type (struct _zend_fcall_info*)
	args.push_back(Type::getInt32Ty(llvm_ctx.context));
	llvm_ctx._zend_execute_internal = new GlobalVariable(
			*llvm_ctx.module,
			PointerType::getUnqual(
				FunctionType::get(
					Type::getVoidTy(llvm_ctx.context),
					ArrayRef<Type*>(args),
					false)),
			false,
			GlobalVariable::ExternalLinkage,
			0,
			ZEND_JIT_SYM("zend_execute_internal"));
	llvm_ctx.engine->addGlobalMapping(llvm_ctx._zend_execute_internal, (void*)zend_execute_internal);

	if (!op_array) {
		zend_hash_init(&llvm_ctx.functions, 16, NULL, NULL, 0);
	}

	return SUCCESS;
}
/* }}} */

/* {{{ static int zend_jit_codegen_end_module */
static int zend_jit_codegen_end_module(zend_jit_context *ctx, zend_op_array *op_array TSRMLS_DC)
{
	zend_llvm_ctx &llvm_ctx = *(zend_llvm_ctx*)ctx->codegen_ctx;
	const void *_entry;

#if (LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR >= 4)
	llvm_ctx.engine->finalizeObject();
#endif

	if (!op_array) {
		zend_string *key;
		void *ptr;

		ZEND_HASH_FOREACH_STR_KEY_PTR(&llvm_ctx.functions, key, ptr) {
			zend_op_array *op_array = (zend_op_array*)ptr;
			_entry = (const void *)llvm_ctx.engine->getPointerToFunction(llvm_ctx.engine->FindFunctionNamed(key->val));
			ZEND_ASSERT(_entry != 0);
            op_array->opcodes[0].handler = _entry;

			if ((op_array->fn_flags & ZEND_ACC_HAS_TYPE_HINTS) == 0) {
				// RECV and RECV_INIT opcodes may be skipped
    	        int n = 0;
	            while (op_array->opcodes[n].opcode == ZEND_RECV ||
    	               op_array->opcodes[n].opcode == ZEND_RECV_INIT) {
	        	    n++;
	    	        op_array->opcodes[n].handler = _entry;
				}
			}
		} ZEND_HASH_FOREACH_END();

		zend_hash_destroy(&llvm_ctx.functions);
	} else {
		_entry = (const void *)llvm_ctx.engine->getPointerToFunction(llvm_ctx.function);
		ZEND_ASSERT(_entry != 0);
		op_array->opcodes[0].handler = _entry;
	}

#if ZEND_LLVM_DEBUG & ZEND_LLVM_DEBUG_DUMP
	if (ZCG(accel_directives).jit_debug & (JIT_DEBUG_DUMP_ASM|JIT_DEBUG_DUMP_ASM_WITH_SSA)) {
		PassManager APM;

#if (LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR < 5)
		APM.add(new DataLayout(*llvm_ctx.engine->getDataLayout()));
#endif

//		Target->setAsmVerbosityDefault(true);
		formatted_raw_ostream FOS(llvm::errs());
    	if (llvm_ctx.target->addPassesToEmitFile(APM, FOS, TargetMachine::CGFT_AssemblyFile, true)) {
			fprintf(stderr, "target does not support assembler output!\n");
	    }
		APM.run(*llvm_ctx.module);
	}
#endif

	// FIXME: keep object to be registered with GDB
	if (ZCG(accel_directives).jit_debug & (JIT_DEBUG_GDB | JIT_DEBUG_OPROFILE)) {
		llvm_ctx.function->deleteBody();
	} else {
		delete llvm_ctx.engine;
	}

	delete llvm_ctx.target;
	delete (zend_llvm_ctx*)ctx->codegen_ctx;

	return SUCCESS;
}
/* }}} */

/* {{{ int zend_opline_supports_jit */
int zend_opline_supports_jit(zend_op_array    *op_array,
                             zend_op          *opline)
{
	switch (opline->opcode) {
		case ZEND_NOP:
		case ZEND_ADD:
		case ZEND_SUB:
		case ZEND_MUL:
		case ZEND_DIV:
		case ZEND_MOD:
		case ZEND_SL:
		case ZEND_SR:
		case ZEND_CONCAT:
		case ZEND_BW_OR:
		case ZEND_BW_AND:
		case ZEND_BW_XOR:
		case ZEND_BW_NOT:
		case ZEND_BOOL_NOT:
		case ZEND_BOOL:
		case ZEND_CASE:
		case ZEND_IS_IDENTICAL:
		case ZEND_IS_NOT_IDENTICAL:
		case ZEND_IS_EQUAL:
		case ZEND_IS_NOT_EQUAL:
		case ZEND_IS_SMALLER:
		case ZEND_IS_SMALLER_OR_EQUAL:
		case ZEND_QM_ASSIGN:
		case ZEND_JMP:
		case ZEND_JMPZ:
		case ZEND_JMPNZ:
		case ZEND_JMPZNZ:
		case ZEND_SEND_VAL:
		case ZEND_SEND_VAL_EX:
		case ZEND_SEND_VAR:
		case ZEND_SEND_VAR_EX:
		case ZEND_SEND_REF:
		case ZEND_SEND_VAR_NO_REF:
		case ZEND_RECV:
		case ZEND_RECV_INIT:
		case ZEND_RETURN:
#if 0 // TODO: reimplement FAST_CONCATR and ROPE_* ???
		case ZEND_ADD_STRING:
		case ZEND_ADD_CHAR:
		case ZEND_ADD_VAR:
#endif
		case ZEND_ECHO:
		case ZEND_FREE:
		case ZEND_TYPE_CHECK:
//???		case ZEND_INIT_ARRAY:
//???		case ZEND_ADD_ARRAY_ELEMENT:
//???		case ZEND_FE_FETCH_R:
//???		case ZEND_FE_FETCH_RW:
//???		case ZEND_ISSET_ISEMPTY_PROP_OBJ:
		case ZEND_ISSET_ISEMPTY_DIM_OBJ:
		case ZEND_INIT_FCALL:
		case ZEND_PRE_INC:
		case ZEND_PRE_DEC:
		case ZEND_POST_INC:
		case ZEND_POST_DEC:
		case ZEND_FETCH_DIM_R:
		case ZEND_FETCH_DIM_IS:
		case ZEND_FETCH_DIM_W:
		case ZEND_FETCH_DIM_RW:
		case ZEND_ASSIGN_DIM:
			return 1;
//???		case ZEND_FETCH_CONSTANT:
//???			return (OP1_OP_TYPE() == IS_UNUSED && OP2_OP_TYPE() == IS_CONST);
		case ZEND_ASSIGN:
			return (OP1_OP_TYPE() == IS_CV);
//???		case ZEND_FETCH_OBJ_W:
//???		case ZEND_FETCH_OBJ_RW:
//???		case ZEND_FETCH_OBJ_R:
		case ZEND_ASSIGN_OBJ:
			return (OP1_OP_TYPE() != IS_VAR);
		case ZEND_ASSIGN_ADD:
		case ZEND_ASSIGN_SUB:
		case ZEND_ASSIGN_MUL:
		case ZEND_ASSIGN_DIV:
//???		case ZEND_ASSIGN_MOD:
//???		case ZEND_ASSIGN_SL:
//???		case ZEND_ASSIGN_SR:
		case ZEND_ASSIGN_CONCAT:
//???		case ZEND_ASSIGN_BW_OR:
//???		case ZEND_ASSIGN_BW_AND:
//???		case ZEND_ASSIGN_BW_XOR:
			return opline->extended_value != ZEND_ASSIGN_OBJ &&
			       (opline->extended_value != ZEND_ASSIGN_DIM || !OP1_MAY_BE(MAY_BE_OBJECT));
		case ZEND_DO_FCALL:
		case ZEND_DO_ICALL:
		case ZEND_DO_UCALL:
		case ZEND_DO_FCALL_BY_NAME:
			return 1;
		case ZEND_OP_DATA:
			zend_opline_supports_jit(op_array, opline - 1);
		default:
			return 0;
	}
	return 0;
}
/* }}} */

/* {{{ int zend_opline_supports_reg_alloc */
int zend_opline_supports_reg_alloc(zend_op_array    *op_array,
                                   zend_op          *opline,
                                   zend_jit_ssa_var *ssa_var)
{
	if (zend_opline_supports_jit(op_array, opline)) {
		switch (opline->opcode) {
			case ZEND_DO_FCALL:
			case ZEND_DO_ICALL:
			case ZEND_DO_UCALL:
			case ZEND_DO_FCALL_BY_NAME:
			case ZEND_ASSIGN_OBJ:
			case ZEND_RECV:
			case ZEND_RECV_INIT:
				return 0;
			case ZEND_FETCH_DIM_W:
			case ZEND_FETCH_DIM_RW:
			case ZEND_FETCH_DIM_UNSET:
			case ZEND_FETCH_DIM_FUNC_ARG:
			case ZEND_FETCH_OBJ_W:
			case ZEND_FETCH_OBJ_RW:
			case ZEND_FETCH_OBJ_UNSET:
			case ZEND_FETCH_OBJ_FUNC_ARG:
				if (ssa_var->var == EX_VAR_TO_NUM(opline->result.var)) {
					return 0;
				}
				break;
			default:
				break;
		}
		return 1;
	}
	return 0;
}
/* }}} */

void zend_jit_mark_reg_zvals(zend_op_array *op_array) /* {{{ */
{
	zend_jit_func_info    *info         = JIT_DATA(op_array);
//	zend_jit_basic_block  *block        = info->cfg.block;
	zend_jit_ssa_op       *ssa_op       = info->ssa.op;
	zend_jit_ssa_var      *ssa_var      = info->ssa.var;
	zend_jit_ssa_var_info *ssa_var_info = info->ssa_var_info;
	int                    ssa_vars     = info->ssa.vars;
	int i, j;

	for (i = 0; i < ssa_vars; i++) {
	    /* variables of concrete types or NULL/FALSE/TRUE may be kept in regs */
		if (has_concrete_type(ssa_var_info[i].type) ||
		    ((ssa_var_info[i].type & (MAY_BE_NULL|MAY_BE_FALSE|MAY_BE_TRUE)) &&
		     !(ssa_var_info[i].type & (MAY_BE_ANY - (MAY_BE_NULL|MAY_BE_FALSE|MAY_BE_TRUE))))) {

			/* TODO: Something wrong ??? */
			if (!(ssa_var_info[i].type & MAY_BE_ANY)) {
				continue;
			}

			/* PHP references and $this cannot be kept in memory */
			if ((ssa_var_info[i].type & MAY_BE_UNDEF) ||
			    (ssa_var_info[i].type & MAY_BE_REF) ||
		    	(ssa_var[i].var < op_array->last_var && (uint32_t)ssa_var[i].var == op_array->this_var)) {
				continue;
			}

			/* STRING may be interned, ARRAY may be immutable */
			if (ssa_var_info[i].type & (MAY_BE_ARRAY|MAY_BE_STRING)) {
				continue;
			}

			/* TODO: try to enable refcounted later ??? */
			if (ssa_var_info[i].type & (MAY_BE_ARRAY|MAY_BE_STRING|MAY_BE_OBJECT|MAY_BE_RESOURCE)) {
				continue;
			}

			if (ssa_var[i].definition >= 0) {
				if (!zend_opline_supports_reg_alloc(op_array, op_array->opcodes + ssa_var[i].definition, ssa_var + i)) {
					continue;
				}
			}

			ssa_var_info[i].type |= MAY_BE_IN_REG;

			j = ssa_var[i].use_chain;
			while (j >= 0) {
				if (!zend_opline_supports_reg_alloc(op_array, op_array->opcodes + j, ssa_var + i)) {
					ssa_var_info[i].type &= ~MAY_BE_IN_REG;
					break;
				}
				j = next_use(ssa_op, i, j);
			}
		}
	}
}
/* }}} */

/* {{{ int zend_jit_codegen_may_compile */
int zend_jit_codegen_may_compile(zend_op_array *op_array TSRMLS_DC)
{
	if (op_array->fn_flags & ZEND_ACC_GENERATOR) {
		// TODO: LLVM Support for generators
		return 0;
	}
	if (op_array->fn_flags & ZEND_ACC_HAS_FINALLY_BLOCK) {
		// TODO: LLVM Support for finally
		return 0;
	}

	return 1;
}
/* }}} */

/* {{{ int zend_jit_codegen_start_script */
int zend_jit_codegen_start_script(zend_jit_context *ctx TSRMLS_DC)
{
	mprotect(asm_buf, asm_buf->end - (zend_uchar*)asm_buf, PROT_READ | PROT_WRITE);
	if (ZEND_LLVM_MODULE_AT_ONCE) {
		return zend_jit_codegen_start_module(ctx, NULL TSRMLS_CC);
	} else {
		return SUCCESS;
	}
}
/* }}} */

/* {{{ int zend_jit_codegen_end_script */
int zend_jit_codegen_end_script(zend_jit_context *ctx TSRMLS_DC)
{
	int ret;

#if JIT_STAT
	jit_stat.compiled_scripts++;
#endif
	if (ZEND_LLVM_MODULE_AT_ONCE) {
		ret = zend_jit_codegen_end_module(ctx, NULL TSRMLS_CC);
	} else {
		ret = SUCCESS;
	}
	mprotect(asm_buf, asm_buf->end - (zend_uchar*)asm_buf, PROT_READ | PROT_EXEC);
	return ret;
}
/* }}} */

/* {{{ int zend_jit_codegen */
int zend_jit_codegen(zend_jit_context *ctx, zend_op_array *op_array TSRMLS_DC)
{
	if (op_array->fn_flags & ZEND_ACC_GENERATOR) {
		// TODO: LLVM Support for generators
		return FAILURE;
	}
	if (op_array->fn_flags & ZEND_ACC_HAS_FINALLY_BLOCK) {
		// TODO: LLVM Support for finally
		return FAILURE;
	}

	if (!ZEND_LLVM_MODULE_AT_ONCE) {
		if (zend_jit_codegen_start_module(ctx, op_array TSRMLS_CC) != SUCCESS) {
			return FAILURE;
		}
	}

	zend_llvm_ctx &llvm_ctx = *(zend_llvm_ctx*)ctx->codegen_ctx;
	zend_jit_func_info *info = JIT_DATA(op_array);

	llvm_ctx.function = zend_jit_get_func(llvm_ctx, ctx, op_array, info);

	// Create LLVM reference to "execute_data"
	Function::arg_iterator args_i = llvm_ctx.function->arg_begin();
	if (!(info->flags & ZEND_JIT_FUNC_NO_FRAME)) {
		llvm_ctx._execute_data = args_i;
		llvm_ctx._execute_data->setName("execute_data");
		args_i++;
	} else {
		llvm_ctx._execute_data = llvm_ctx.builder.CreateIntToPtr(
			LLVM_GET_LONG(0),
			PointerType::getUnqual(llvm_ctx.zend_execute_data_type));
		llvm_ctx._execute_data->setName("execute_data");
	}
//???	if (info->flags & ZEND_JIT_FUNC_HAS_REG_ARGS) {
//???		llvm_ctx.arg_reg = (Value**)zend_jit_context_calloc(ctx, sizeof(Value*), info->num_args);
//???		int i;
//???
//???		for (i = 0 ; i < info->num_args; i++) {
//???			if (info->arg_info[i].info.type & (MAY_BE_IN_REG|MAY_BE_TMP_ZVAL)) {
//???				llvm_ctx.arg_reg[i] = args_i;
//???				args_i++;
//???			}
//???		}
//???	}
//???	if (info->return_info.type & MAY_BE_TMP_ZVAL) {
//???		llvm_ctx.ret_reg = args_i;
//???		args_i++;
//???	}

	llvm_ctx.function_name = NULL;
	llvm_ctx.inline_level = 0;
	memset(llvm_ctx.stack_slots, 0, sizeof(llvm_ctx.stack_slots));

	void *checkpoint = zend_arena_checkpoint(ctx->arena);

	if (!zend_jit_codegen_ex(ctx, llvm_ctx, op_array)) {
		zend_arena_release(&ctx->arena, checkpoint);
		return FAILURE;
	}

#if ZEND_LLVM_DEBUG & ZEND_LLVM_DEBUG_DUMP
	if (ZCG(accel_directives).jit_debug & JIT_DEBUG_DUMP_SRC_LLVM_IR) {
		llvm_ctx.function->dump();
	}
#endif

#if ZEND_LLVM_DEBUG & ZEND_LLVM_DEBUG_VERIFY_IR
	verifyFunction(*llvm_ctx.function);
#endif

	if ((ZCG(accel_directives).jit_opt & JIT_OPT_LLVM) != JIT_OPT_LLVM_O0) {
		FunctionPassManager FPM(llvm_ctx.module);

#if (LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR < 5)
		FPM.add(new DataLayout(*llvm_ctx.engine->getDataLayout()));
#endif

//		FPM.add(createTypeBasedAliasAnalysisPass());
//		FPM.add(createBasicAliasAnalysisPass());

		FPM.add(createCFGSimplificationPass());
		FPM.add(createInstructionCombiningPass());

//		FPM.add(createPromoteMemoryToRegisterPass());
		FPM.add(createScalarReplAggregatesPass());

//		FPM.add(createEarlyCSEPass());
//		FPM.add(createSimplifyLibCallsPass());

#if 0
		FPM.add(createLowerExpectIntrinsicPass());
#endif

		FPM.add(createJumpThreadingPass());
//		FPM.add(createCorrelatedValuePropagationPass());
		FPM.add(createCFGSimplificationPass());
		FPM.add(createInstructionCombiningPass());
		
//		FPM.add(createTailCallEliminationPass());
//		FPM.add(createCFGSimplificationPass());
		FPM.add(createReassociatePass());

		FPM.add(createLoopRotatePass());
//		FPM.add(createLICMPass());
//		FPM.add(createLoopUnswitchPass());
//		FPM.add(createInstructionCombiningPass());
		FPM.add(createIndVarSimplifyPass());
//		FPM.add(createLoopIdiomPass());
		FPM.add(createLoopDeletionPass());
//		FPM.add(createLoopUnrollPass());
		
//		FPM.add(createGVNPass());
//		FPM.add(createMemCpyOptPass());
//		FPM.add(createSCCPPass());
//		FPM.add(createInstructionCombiningPass());
//		FPM.add(createJumpThreadingPass());
//		FPM.add(createCorrelatedValuePropagationPass());
		FPM.add(createDeadStoreEliminationPass());

//		FPM.add(createAggressiveDCEPass());
//		FPM.add(createCFGSimplificationPass());
		FPM.add(createInstructionCombiningPass());

		FPM.doInitialization();
		FPM.run(*llvm_ctx.function);
		FPM.doFinalization();
	}

#if ZEND_LLVM_DEBUG & ZEND_LLVM_DEBUG_DUMP
	if (ZCG(accel_directives).jit_debug & JIT_DEBUG_DUMP_OPT_LLVM_IR) {
		llvm_ctx.function->dump();
	}
#endif

    if (info->clone_num == 0) {
		if (ZEND_LLVM_MODULE_AT_ONCE) {
			if (zend_hash_str_add_ptr(&llvm_ctx.functions, llvm_ctx.function->getName().data(), llvm_ctx.function->getName().size(), op_array) == NULL) {
				zend_arena_release(&ctx->arena, checkpoint);
				return FAILURE;
			}
		} else {
			if (zend_jit_codegen_end_module(ctx, op_array TSRMLS_CC) != SUCCESS) {
				zend_arena_release(&ctx->arena, checkpoint);
				return FAILURE;
			}
		}
	}

	zend_arena_release(&ctx->arena, checkpoint);
	return SUCCESS;
}
/* }}} */

/* {{{ int zend_jit_codegen_startup */
int zend_jit_codegen_startup(size_t size)
{
#ifdef _WIN32
	/* TODO: It has to be shared memory */
	zend_uchar *p = (zend_uchar*)VirtualAlloc(0, size,
		MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!p) {
		return FAILURE;
	}
#else
	int shared = 1;
	zend_uchar *p;

	if (ZCG(accel_directives).jit_debug & JIT_DEBUG_OPROFILE) {
		// We have to use private (not shared) memory segment to make
		// OProfile recgnize it
		shared = 0;
	}
	do {

# ifdef MAP_HUGETLB
		/* Try to allocate huge pages first to reduce dTLB misses.
		 * OS has to be configured properly
		 * (e.g. https://wiki.debian.org/Hugepages#Enabling_HugeTlbPage)
		 * You may verify huge page usage with the following command:
		 * `grep "Huge" /proc/meminfo`
		 */
		p = (zend_uchar*)mmap(NULL, size,
			PROT_EXEC | PROT_READ | PROT_WRITE,
			(shared ? MAP_SHARED : MAP_PRIVATE) | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
		if (p != MAP_FAILED) {
			break;
		}
# endif
		p = (zend_uchar*)mmap(NULL, size,
			PROT_EXEC | PROT_READ | PROT_WRITE,
			(shared ? MAP_SHARED : MAP_PRIVATE) | MAP_ANONYMOUS, -1, 0);
		if (p == MAP_FAILED) {
			return FAILURE;
		}
	} while (0);
#endif
	asm_buf = (zend_asm_buf*)p;
	asm_buf->base = asm_buf->ptr = p + sizeof(zend_asm_buf);
	asm_buf->end = p + size;

	orig_execute_ex = zend_execute_ex;
	zend_execute_ex = jit_execute_ex;

	InitializeNativeTarget();
	InitializeNativeTargetAsmPrinter();

//	zend_jit_exception_handler(asm_buf);
//	gen_jit_leave_func(asm_buf);
//	gen_jit_leave_method(asm_buf);
//	gen_jit_leave_code(asm_buf);

//???	zend_hash_init(&inline_functions, 16, NULL, NULL, 1);
//???	for (int i = 0; i < sizeof(inline_func_infos)/sizeof(inline_func_infos[0]); i++) {
//???		zend_hash_str_add_ptr(
//???				&inline_functions,
//???				inline_func_infos[i].name,
//???				inline_func_infos[i].name_len,
//???				const_cast<void**>((void * const *)&inline_func_infos[i].inline_func));
//???	}

#ifdef HAVE_OPROFILE
	if (ZCG(accel_directives).jit_debug & JIT_DEBUG_OPROFILE) {
		event_listener = JITEventListener::createOProfileJITEventListener();
	}
#endif

	return SUCCESS;
}
/* }}} */

/* {{{ int zend_jit_codegen_shutdown */
int zend_jit_codegen_shutdown(void)
{
//???	zend_hash_destroy(&inline_functions);
#if JIT_STAT
	if (ZCG(accel_directives).jit_debug & JIT_DEBUG_STAT) {
		fprintf(stderr, "== PHP JIT statistics ==\n");
		fprintf(stderr, "  Compiled scripts:   %ld\n", jit_stat.compiled_scripts);
		fprintf(stderr, "  Compiled op_arrays: %ld\n", jit_stat.compiled_op_arrays);
		fprintf(stderr, "  Compiled clones:    %ld\n", jit_stat.compiled_clones);
		fprintf(stderr, "  Inlined clones:     %ld\n", jit_stat.inlined_clones);
		fprintf(stderr, "  SSA variables:      %ld\n", jit_stat.ssa_vars);
		fprintf(stderr, "  Untyped SSA vars:   %ld\n", jit_stat.untyped_ssa_vars);
		fprintf(stderr, "  Typed SSA vars:     %ld\n", jit_stat.typed_ssa_vars);
		fprintf(stderr, "  SSA vars in regs:   %ld\n", jit_stat.reg_ssa_vars);
		fprintf(stderr, "  Used memory: %ld [bytes]\n", asm_buf->ptr - asm_buf->base);
	}
#endif

#ifdef HAVE_OPROFILE
	if (event_listener) {
		delete event_listener;
	}
#endif

	return SUCCESS;
}
/* }}} */

#ifdef __cplusplus
}
#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * indent-tabs-mode: t
 * End:
 */
