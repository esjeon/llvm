set(LLVM_LINK_COMPONENTS
  Support
  DebugInfoPDB
  )

add_llvm_tool(llvm-pdbdump
  llvm-pdbdump.cpp
  )
