### Build instruction using IBM Open XL compiler/tool-chain
* Make sure that latest IBM Open XL compiler is installed. At the writing of this doc, IBM Open XL 17.1.2.5 is used.
* Build script internally uses python, so install python3 also.
* Export the below env settings.
  ```bash
  ulimit -m unlimited
  ulimit -d unlimited
  ulimit -f unlimited
  export OBJECT_MODE=64
  export CC="/opt/IBM/openxlC/17.1.2/bin/ibm-clang  -pthread -m64  -g " 
  export CXX="/opt/IBM/openxlC/17.1.2/bin/ibm-clang++_r  -pthread -m64 -g "
  export CFLAGS="-I/usr/local/include -D_ALL_SOURCE -mcmodel=large  -Wno-deprecate-lax-vec-conv-all  -Wno-unused-but-set-variable -Wno-unused-command-line-argument -maltivec -mvsx  -Wno-unused-variable -Wno-unused-parameter -Wno-sign-compare "
  export CXXFLAGS="-I/usr/local/include -D_ALL_SOURCE -mcmodel=large -Wno-deprecate-lax-vec-conv-all -Wno-unused-but-set-variable -Wno-unused-command-line-argument -maltivec -mvsx  -Wno-unused-variable -Wno-unused-parameter -Wno-sign-compare"
  ``` 
- Invoke the build script. 
  ```bash
  ./build.sh \
	--config RelWithDebInfo\
	--build_shared_lib \
	--skip_submodule_sync \
	--cmake_extra_defines CMAKE_INSTALL_PREFIX=/usr/local  \
	--parallel \
	--allow_running_as_root
  ```
- Change the install path CMAKE_INSTALL_PREFIX based on your requirement
  
### Build instruction using gcc compiler/tool-chain
* Make sure that gcc/g++ compiler is installed.
* Build script internally uses python, so install python3 also.
* Export the below env settings.
  ```bash
  ulimit -m unlimited
  ulimit -d unlimited
  ulimit -f unlimited
  export OBJECT_MODE=64
  export CC="/opt/freeware/bin/gcc -pthread -maix64 "
  export CXX="/opt/freeware/bin/g++ -pthread -maix64 "
  export CPPFLAGS="-I/opt/freeware/include -maix64 "
  export CFLAGS="${CPPFLAGS} -DFLATBUFFERS_LOCALE_INDEPENDENT=0 -maltivec -mvsx -Wl,-bbigtoc -Wno-unused-variable -Wno-unused-parameter -Wno-sign-compare -fno-extern-tls-init  -Wl,-berok "
  export CXXFLAGS="${CPPFLAGS} -DFLATBUFFERS_LOCALE_INDEPENDENT=0 -maltivec -mvsx -Wl,-bbigtoc -Wno-unused-variable -Wno-unused-parameter -Wno-sign-compare -fno-extern-tls-init -Wl,-berok"
  export LDFLAGS="-L/opt/freeware/lib/pthread -L/opt/freeware/lib64 -L/opt/freeware/lib -lpython3.9"
  ``` 
- Invoke the build script. 
  ```bash
  ./build.sh \
	--config RelWithDebInfo\
	--build_shared_lib \
	--skip_submodule_sync \
	--cmake_extra_defines CMAKE_INSTALL_PREFIX=/usr/local  \
	--parallel \
	--allow_running_as_root
  ```
- Change the install path CMAKE_INSTALL_PREFIX based on your requirement
- python binding can also be compiled using the option  **--enable_pybind** in build.sh

  
