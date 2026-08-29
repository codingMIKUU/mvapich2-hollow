## Copyright (c) 2001-2022, The Ohio State University. All rights
## reserved.
##
## This file is part of the MVAPICH2 software package developed by the
## team members of The Ohio State University's Network-Based Computing
## Laboratory (NBCL), headed by Professor Dhabaleswar K. (DK) Panda.
##
## For detailed copyright and licensing information, please refer to the
## copyright file COPYRIGHT in the top level MVAPICH2 directory.
##

## todo: provide configure args for path to nvcc and specification of -arch or
## 	-maxrregcount values

AM_NVCCFLAGS = $(NVCCFLAGS)

if BUILD_MRAIL_CUDA_SM_86
AM_NVCCFLAGS += -gencode=arch=compute_86,code=compute_86
endif

if BUILD_MRAIL_CUDA_SM_80
AM_NVCCFLAGS += -gencode=arch=compute_80,code=compute_80
endif

if BUILD_MRAIL_CUDA_SM_75
AM_NVCCFLAGS += -gencode=arch=compute_75,code=compute_75
endif

if BUILD_MRAIL_CUDA_SM_70
AM_NVCCFLAGS += -gencode=arch=compute_70,code=compute_70
endif

if BUILD_MRAIL_CUDA_SM_62
AM_NVCCFLAGS += -gencode=arch=compute_62,code=compute_62
endif

if BUILD_MRAIL_CUDA_SM_53
AM_NVCCFLAGS += -gencode=arch=compute_53,code=compute_53
endif

if BUILD_MRAIL_CUDA_SM_37
AM_NVCCFLAGS += -gencode=arch=compute_37,code=compute_37
endif

NVCC = nvcc
NVCFLAGS = -cuda -ccbin $(CXX) $(AM_NVCCFLAGS)

SUFFIXES += .cu .cpp
.cu.cpp:
	$(NVCC) $(NVCFLAGS) $(INCLUDES) $(CPPFLAGS) --output-file $@.ii $<
	mv $@.ii $@

noinst_LTLIBRARIES += lib/lib@MPILIBNAME@_cuda_osu.la
lib_lib@MPILIBNAME@_cuda_osu_la_SOURCES =                   \
	    src/mpid/ch3/channels/mrail/src/cuda/pack_unpack.cu

lib_lib@MPILIBNAME@_cuda_osu_la_CXXFLAGS = $(AM_CXXFLAGS)
### use extra flags if host compiler is PGI
if BUILD_USE_PGI
lib_lib@MPILIBNAME@_cuda_osu_la_CXXFLAGS += --nvcchost --no_preincludes
endif
lib_lib@MPILIBNAME@_cuda_osu_la_LIBADD = -lstdc++

lib_lib@MPILIBNAME@_la_LIBADD += lib/lib@MPILIBNAME@_cuda_osu.la

CLEANFILES += src/mpid/ch3/channels/mrail/src/cuda/*.cpp
