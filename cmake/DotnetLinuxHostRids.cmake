# ge_dotnet_linux_host_rids(<dotnet_root> <arch> <out_var>)
#
# Lists the runtime identifiers of the Linux host packs installed under
# <dotnet_root>/packs for <arch> (x64, arm64, x86), in the order to search
# them for libnethost. Microsoft's builds ship the portable pack (linux-x64);
# distro source builds ship one named for the distro's own RID instead
# (fedora.44-x64, ubuntu.24.04-x64). The portable RID comes first whether or
# not its pack exists, then every other glibc pack for the architecture;
# linux-musl packs are skipped because a musl libnethost does not load on glibc.
function(ge_dotnet_linux_host_rids dotnet_root arch out_var)
  set(_prefix "${dotnet_root}/packs/Microsoft.NETCore.App.Host.")
  string(LENGTH "${_prefix}" _prefix_length)
  file(GLOB _packs LIST_DIRECTORIES true "${_prefix}*-${arch}")
  list(SORT _packs)
  set(_rids "linux-${arch}")
  foreach(_pack IN LISTS _packs)
    string(SUBSTRING "${_pack}" ${_prefix_length} -1 _rid)
    if(NOT _rid MATCHES "^linux-")
      list(APPEND _rids "${_rid}")
    endif()
  endforeach()
  set(${out_var} ${_rids} PARENT_SCOPE)
endfunction()
