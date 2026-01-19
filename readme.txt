/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

Required libraries:
1) Boost version 1.39 or later
2) TBB (Threading Building Blocks) version 2.1 or later
3) MSM (Meta State Machine) version 1.2 or later
4) Boost Logging Library v2

Environment variables required:
2) $(MSM) - path to the MSM library, for example 'E:\develop\Msm120-RC1\msm\'
3) $(BOOST_INCLUDE) - path to the Boost library, for example 'E:\develop\boost_1_39_0\boost_1_39_0\'
4) $(TBB_HOME) - path to the TBB library, for example 'E:\develop\tbb21_20080605oss'

Supported compilers:
1) MSVC 2005

How to build:
1) Download and build required libraries
2) Configure environment variables
3) Open OrderProcessor.sln and build the library project

How to execute tests:
1) Open OrderProcessor.sln and build the test project
2) Copy boost_regex-vc80-mt-1_39.dll, tbb.dll, tbb_debug.dll, tbbmalloc.dll, 
  tbbmalloc_debug.dll libraries to the <COP folder>\engine\test\out\ folder
3) Run the exe