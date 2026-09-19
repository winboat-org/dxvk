cs_test_deps = [dxvk_dep, dxbc_spirv_dep, vkcommon_dep, dependency('threads')]
if platform == 'windows'
  foreach name : ['gdi32', 'user32', 'ole32', 'oleaut32', 'version', 'advapi32', 'shell32', 'cfgmgr32']
    cs_test_deps += cpp.find_library(name)
  endforeach
endif
cs_failure_test = executable('test_cs_failure', 'test_cs_failure.cpp',
  dependencies: cs_test_deps,
  include_directories: [dxvk_include_path, include_directories('../subprojects/dxbc-spirv')])
test('cs_failure', cs_failure_test, timeout: 60)

helios_image_import_test = executable('test_helios_image_import',
  'test_helios_image_import.cpp', include_directories: dxvk_include_path)
test('helios_image_import', helios_image_import_test)
