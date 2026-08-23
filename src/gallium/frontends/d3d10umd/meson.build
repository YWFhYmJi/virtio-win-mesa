inc_d3d10umd = include_directories('.')
_c_args_d3d10umd = []
d3d10umd_files = files(
  'Adapter.cpp',
  'D3D9Adapter.cpp',
  'D3D9Common.cpp',
  'D3D9Draw.cpp',
  'D3D9NineShader.c',
  'D3D9Resource.cpp',
  'D3D9ShaderTranslate.cpp',
  'D3D9State.cpp',
  'Debug.cpp',
  'Device.cpp',
  'Draw.cpp',
  'DxgiFns.cpp',
  'Format.cpp',
  'InputAssembly.cpp',
  'OutputMerger.cpp',
  'Query.cpp',
  'Rasterizer.cpp',
  'Resource.cpp',
  'Shader.cpp',
  'ShaderDump.cpp',
  'ShaderParse.c',
  'ShaderTGSI.c',
  'D3DKMT.cpp',
  'gdikmt_d3dddi.cpp'
)

if cc.get_argument_syntax() == 'gcc'
  _c_args_d3d10umd += '-Wno-unknown-pragmas'
endif

if not with_gallium_yttrium
  d3d10umd_files += files('YttriumStubs.cpp')
endif

libd3d10umd = static_library(
  'd3d10umd',
  d3d10umd_files,
  c_args : _c_args_d3d10umd,
  cpp_args : _c_args_d3d10umd,
  include_directories : [
    inc_include, inc_src, inc_gallium, inc_gallium_aux, inc_winddk,
  ],
  dependencies: idep_mesautilformat,
)
