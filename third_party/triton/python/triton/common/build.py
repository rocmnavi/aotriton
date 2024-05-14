import contextlib
import functools
import io
import os
import shutil
import subprocess
import sys
import sysconfig

import setuptools


class CurrentBuildTarget:
    arch = None
    aot_arch = None
    @staticmethod
    def is_aot():
        return CurrentBuildTarget.aot_arch is not None

    @staticmethod
    def configure_arch(arch : dict):
        CurrentBuildTarget.arch = dict(arch)

    @staticmethod
    def configure_aot_arch(aot_arch : str):
        CurrentBuildTarget.aot_arch = aot_arch

    @staticmethod
    def is_hip():
        aot_arch = CurrentBuildTarget.aot_arch
        if aot_arch is not None:
            if aot_arch.startswith('MI') or aot_arch.startswith('Radeon'):
                return True
            else:
                return False
        arch = CurrentBuildTarget.arch
        if arch is not None and isinstance(arch, dict):
            gfx_triple = arch.get('gfx_triple', '')
            return gfx_triple == 'amdgcn-amd-amdhsa'
        # Auto detection
        try:
            import torch
        except ImportError:
            raise ImportError("Triton requires PyTorch to be installed")
        return torch.version.hip is not None

    @staticmethod
    def get_arch_default_num_warps():
        if CurrentBuildTarget.is_hip():
            return CurrentBuildTarget.arch['num_warps']
        raise NotImplementedError(f"CurrentBuildTarget.get_arch_default_num_warps not implemented for {CurrentBuildTarget.aot_arch} {CurrentBuildTarget.arch}")

    @staticmethod
    def get_arch_default_num_stages():
        if CurrentBuildTarget.is_hip():
            return CurrentBuildTarget.arch['num_stages']
        raise NotImplementedError(f"CurrentBuildTarget.get_arch_default_num_stages not implemented for {CurrentBuildTarget.aot_arch} {CurrentBuildTarget.arch}")

is_hip = CurrentBuildTarget.is_hip


@functools.lru_cache()
def libcuda_dirs():
    libs = subprocess.check_output(["/sbin/ldconfig", "-p"]).decode()
    # each line looks like the following:
    # libcuda.so.1 (libc6,x86-64) => /lib/x86_64-linux-gnu/libcuda.so.1
    locs = [line.split()[-1] for line in libs.splitlines() if "libcuda.so" in line]
    dirs = [os.path.dirname(loc) for loc in locs]
    msg = 'libcuda.so cannot found!\n'
    if locs:
        msg += 'Possible files are located at %s.' % str(locs)
        msg += 'Please create a symlink of libcuda.so to any of the file.'
    else:
        msg += 'Please make sure GPU is setup and then run "/sbin/ldconfig"'
        msg += ' (requires sudo) to refresh the linker cache.'
    assert any(os.path.exists(os.path.join(path, 'libcuda.so')) for path in dirs), msg
    return dirs


@functools.lru_cache()
def rocm_path_dir():
    default_path = os.path.join(os.path.dirname(__file__), "..", "third_party", "hip")
    # Check if include files have been populated locally.  If so, then we are 
    # most likely in a whl installation and he rest of our libraries should be here
    if (os.path.exists(default_path+"/include/hip/hip_runtime.h")):
        return default_path
    else:
        return os.getenv("ROCM_PATH", default="/opt/rocm")


@contextlib.contextmanager
def quiet():
    old_stdout, old_stderr = sys.stdout, sys.stderr
    sys.stdout, sys.stderr = io.StringIO(), io.StringIO()
    try:
        yield
    finally:
        sys.stdout, sys.stderr = old_stdout, old_stderr


@functools.lru_cache()
def cuda_include_dir():
    base_dir = os.path.join(os.path.dirname(__file__), os.path.pardir)
    cuda_path = os.path.join(base_dir, "third_party", "cuda")
    return os.path.join(cuda_path, "include")


def _build(name, src, srcdir):
    if is_hip():
        hip_lib_dir = os.path.join(rocm_path_dir(), "lib")
        hip_include_dir = os.path.join(rocm_path_dir(), "include")
    else:
        cuda_lib_dirs = libcuda_dirs()
        cu_include_dir = cuda_include_dir()
    suffix = sysconfig.get_config_var('EXT_SUFFIX')
    so = os.path.join(srcdir, '{name}{suffix}'.format(name=name, suffix=suffix))
    # try to avoid setuptools if possible
    cc = os.environ.get("CC")
    if cc is None:
        # TODO: support more things here.
        clang = shutil.which("clang")
        gcc = shutil.which("gcc")
        cc = gcc if gcc is not None else clang
        if cc is None:
            raise RuntimeError("Failed to find C compiler. Please specify via CC environment variable.")
    # This function was renamed and made public in Python 3.10
    if hasattr(sysconfig, 'get_default_scheme'):
        scheme = sysconfig.get_default_scheme()
    else:
        scheme = sysconfig._get_default_scheme()
    # 'posix_local' is a custom scheme on Debian. However, starting Python 3.10, the default install
    # path changes to include 'local'. This change is required to use triton with system-wide python.
    if scheme == 'posix_local':
        scheme = 'posix_prefix'
    py_include_dir = sysconfig.get_paths(scheme=scheme)["include"]

    if is_hip():
        ret = subprocess.check_call([
            cc, src, f"-I{hip_include_dir}", f"-I{py_include_dir}", f"-I{srcdir}", "-shared", "-fPIC",
            f"-L{hip_lib_dir}", "-lamdhip64", f"-Wl,-rpath,{hip_lib_dir}", "-o", so
        ])
    else:
        cc_cmd = [
            cc, src, "-O3", f"-I{cu_include_dir}", f"-I{py_include_dir}", f"-I{srcdir}", "-shared", "-fPIC", "-lcuda",
            "-o", so
        ]
        cc_cmd += [f"-L{dir}" for dir in cuda_lib_dirs]
        ret = subprocess.check_call(cc_cmd)

    if ret == 0:
        return so
    # fallback on setuptools
    extra_compile_args = []
    library_dirs = cuda_lib_dirs
    include_dirs = [srcdir, cu_include_dir]
    libraries = ['cuda']
    # extra arguments
    extra_link_args = []
    # create extension module
    ext = setuptools.Extension(
        name=name,
        language='c',
        sources=[src],
        include_dirs=include_dirs,
        extra_compile_args=extra_compile_args + ['-O3'],
        extra_link_args=extra_link_args,
        library_dirs=library_dirs,
        libraries=libraries,
    )
    # build extension module
    args = ['build_ext']
    args.append('--build-temp=' + srcdir)
    args.append('--build-lib=' + srcdir)
    args.append('-q')
    args = dict(
        name=name,
        ext_modules=[ext],
        script_args=args,
    )
    with quiet():
        setuptools.setup(**args)
    return so
