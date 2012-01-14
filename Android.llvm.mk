#
# Centralized place for LLVM configuration.
#

ifeq ($(strip $(MESA_LLVM)),true)

major := $(word 1, $(subst ., , $(PLATFORM_VERSION)))

ifneq ($(filter 3 4, $(major)),)

# LLVM 3.0svn
LLVM_VERSION := 0x0300

llvm_static_libraries_1 := \
	libLLVMMCJIT \
	libLLVMRuntimeDyld \
	libLLVMObject \
	libLLVMMCDisassembler \
	libLLVMLinker \
	libLLVMipo \
	libLLVMInterpreter \
	libLLVMInstrumentation \
	libLLVMJIT \
	libLLVMExecutionEngine \
	libLLVMBitWriter

llvm_static_libraries_x86 := \
	libLLVMX86Disassembler \
	libLLVMX86AsmParser \
	libLLVMX86CodeGen \
	libLLVMX86Desc \
	libLLVMSelectionDAG \
	libLLVMX86AsmPrinter \
	libLLVMX86Utils \
	libLLVMX86Info

llvm_static_libraries_arm := \
	libLLVMARMDisassembler \
	libLLVMARMCodeGen \
	libLLVMARMDesc \
	libLLVMSelectionDAG \
	libLLVMARMAsmPrinter \
	libLLVMARMInfo

llvm_static_libraries_2 += \
	libLLVMAsmPrinter \
	libLLVMMCParser \
	libLLVMCodeGen \
	libLLVMScalarOpts \
	libLLVMInstCombine \
	libLLVMTransformUtils \
	libLLVMipa \
	libLLVMAsmParser \
	libLLVMArchive \
	libLLVMBitReader \
	libLLVMAnalysis \
	libLLVMTarget \
	libLLVMMC \
	libLLVMCore \
	libLLVMSupport

endif # major 3 or 4

ifeq ($(llvm_static_libraries_$(TARGET_ARCH)),)
$(error LLVM not available for Android $(PLATFORM_VERSION) on $(TARGET_ARCH))
endif

endif # MESA_LLVM

ifeq ($(strip $(MESA_LLVM)),true)

# this is a static library
ifeq ($(strip $(LOCAL_MODULE_CLASS)),STATIC_LIBRARIES)
LOCAL_CFLAGS += -DHAVE_LLVM=$(LLVM_VERSION)

LLVM_ROOT_PATH := external/llvm
include $(LLVM_ROOT_PATH)/llvm-device-build.mk
endif

# this is a shared library
ifeq ($(strip $(LOCAL_MODULE_CLASS)),SHARED_LIBRARIES)
LOCAL_SHARED_LIBRARIES += libstlport

LOCAL_STATIC_LIBRARIES += \
	$(llvm_static_libraries_1) \
	$(llvm_static_libraries_$(TARGET_ARCH)) \
	$(llvm_static_libraries_2)
endif

endif # MESA_LLVM
