# PSGL buffer replacement heap regression

This is the unchanged buffer-only replay from t_52b7440d. It creates no shaders
and submits no draws. It repeatedly replaces array and element-buffer storage,
then replaces the element buffer from 24 to 28 bytes and deletes all buffers.
Success is the single `CHURN_OK` marker and clean process completion.

Before the fix, LP64 crashes in `heapFree` through `rsxFree` while releasing the
old storage for the final 28-byte replacement. The same ILP32 sequence completes.
This was reproduced with both pristine 0d3230ec libPSGL and installed v0.26.153.
The allocator could create a 24-byte LP64 block, although a free block needs 32
bytes. Its predecessor link overlaps the following block's boundary tag.

Build with the PPU CMake toolchain and the candidate runtime installed in a
private SDK prefix. Both ABI targets are in the regression manifest. A candidate
heap object can also be linked directly before the SDK archives for isolated
validation; replacing only libPSGL does not replace the allocator.
