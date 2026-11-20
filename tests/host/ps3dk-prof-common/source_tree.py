"""Copy the runtime and its public headers, preserving SDK-relative includes.

Mutation tests must compile the same sources as the control. Copying src alone
loses headers reached by relative includes (notably prof_rolling.h).
"""
import shutil


def copy_runtime(source, destination):
    copied = destination / 'sdk/libps3dkprof/src'
    shutil.copytree(source, copied)
    shutil.copytree(source.parents[1] / 'include/ps3dk',
                    destination / 'sdk/include/ps3dk')
    return copied
