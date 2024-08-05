# Build instruction using IBM OpenXL compier
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
- Invoke the building using below.
  ```bash
  ./build.sh \
	      --config RelWithDebInfo\
        --build_shared_lib \
        --skip_submodule_sync \
        --cmake_extra_defines CMAKE_INSTALL_PREFIX=/usr/local  \
        --parallel \
        --allow_running_as_root
  ``` 
# Build instruction using gcc compier
  
