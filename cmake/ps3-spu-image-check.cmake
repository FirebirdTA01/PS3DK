# ps3-spu-image-check.cmake - run by ps3_add_spu_image (ps3-self.cmake) as
#   cmake -DPS3_SPU_CHECK_TOOL=<spu-elf-to-ppu-obj> -DPS3_SPU_ELF=<image>
#         -P ps3-spu-image-check.cmake
# right after the SPU link.  The image is linked under its final name (the
# linker records the -o name in .note.spu_name, so a scratch name would be
# embedded), which means a refused image is already at the output path.
# Remove it on refusal, so the next build re-links instead of picking up an
# image that did not pass.
if(NOT PS3_SPU_CHECK_TOOL OR NOT PS3_SPU_ELF)
    message(FATAL_ERROR "ps3-spu-image-check: PS3_SPU_CHECK_TOOL and PS3_SPU_ELF are required")
endif()
execute_process(
    COMMAND "${PS3_SPU_CHECK_TOOL}" no-ppu-refs --spu-elf "${PS3_SPU_ELF}"
    RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    file(REMOVE "${PS3_SPU_ELF}")
    message(FATAL_ERROR "ps3-spu-image-check: ${PS3_SPU_ELF} refused (${_rc}); removed")
endif()
