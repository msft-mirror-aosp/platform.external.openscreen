# Copyright 2018 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Recipe for building and running tests for Open Screen stand-alone."""

from __future__ import annotations

from recipe_engine import recipe_api
from recipe_engine.config_types import Path

from dataclasses import dataclass

from recipe_engine.recipe_api import RecipeScriptApi
from recipe_engine.recipe_test_api import RecipeTestApi

from RECIPE_MODULES.build import code_coverage, profiles
from RECIPE_MODULES.depot_tools import (
    bot_update,
    depot_tools,
    gclient,
    git,
    gsutil,
    osx_sdk,
    tryserver,
)
from RECIPE_MODULES.recipe_engine import (
    buildbucket,
    cas,
    context,
    file,
    json,
    path,
    platform,
    properties,
    step,
    swarming,
)


@dataclass
class DEPS(RecipeScriptApi):
  bot_update: bot_update.API
  buildbucket: buildbucket.API
  cas: cas.API
  code_coverage: code_coverage.API
  context: context.API
  depot_tools: depot_tools.API
  file: file.API
  gclient: gclient.API
  git: git.API
  gsutil: gsutil.API
  json: json.API
  osx_sdk: osx_sdk.API
  path: path.API
  platform: platform.API
  profiles: profiles.API
  properties: properties.API
  step: step.API
  swarming: swarming.API
  tryserver: tryserver.API


@dataclass
class TEST_DEPS(RecipeTestApi):
  buildbucket: buildbucket.TEST_API
  code_coverage: code_coverage.TEST_API
  context: context.TEST_API
  file: file.TEST_API
  gclient: gclient.TEST_API
  json: json.TEST_API
  path: path.TEST_API
  platform: platform.TEST_API
  properties: properties.TEST_API
  step: step.TEST_API
  swarming: swarming.TEST_API
  tryserver: tryserver.TEST_API


# Open Screen specific paths and repository information.
BUILD_CONFIG = 'Default'
UNIT_TEST_BINARY_NAME = 'openscreen_unittests'
E2E_TEST_BINARY_NAME = 'e2e_tests'
CAST_E2E_TEST_SCRIPT_NAME = 'standalone_e2e.py'
CAST_SENDER_BINARY_NAME = 'cast_sender'
CAST_RECEIVER_BINARY_NAME = 'cast_receiver'
DEFAULT_BUILD_TARGETS = [
    'gn_all',
    UNIT_TEST_BINARY_NAME,
    E2E_TEST_BINARY_NAME,
    'fuzzer_tests_all',
    CAST_SENDER_BINARY_NAME,
    CAST_RECEIVER_BINARY_NAME,
]
BUILD_TARGETS = DEFAULT_BUILD_TARGETS
OPENSCREEN_REPO = 'https://chromium.googlesource.com/openscreen'

# List of dimensions used for starting swarming on ARM64.
SWARMING_DIMENSIONS = {'cpu': 'arm64', 'os': 'Ubuntu-24'}

# LUCI pool information.
FLEX_TRY_POOL = 'luci.flex.try'
FLEX_CI_POOL = 'luci.flex.ci'
POOL_DIMENSION = 'pool'


class RepositoryPaths:
  """Container for checkout_path dependent repository paths."""

  def __init__(self, api: DEPS, source_dir: Path):
    self.api = api
    self.checkout_path = source_dir
    self.output_path = self.checkout_path / 'out' / BUILD_CONFIG
    self.unit_test_binary_path = self._generate_binary_path(
        UNIT_TEST_BINARY_NAME)
    self.e2e_test_binary_path = self._generate_binary_path(
        E2E_TEST_BINARY_NAME)
    self.cast_e2e_test_script_path = (self.checkout_path / 'cast' /
                                      CAST_E2E_TEST_SCRIPT_NAME)
    self.cast_sender_binary_path = self._generate_binary_path(
        CAST_SENDER_BINARY_NAME)
    self.cast_receiver_binary_path = self._generate_binary_path(
        CAST_RECEIVER_BINARY_NAME)
    self.test_data_path = self.checkout_path / 'test' / 'data'
    exe_suffix = '.exe' if api.platform.is_win else ''
    self.ninja_path = (self.checkout_path / 'third_party' / 'ninja' /
                       f'ninja{exe_suffix}')

  def _generate_binary_path(self, binary_name: str) -> Path:
    """Returns the full output path for a binary, appending .exe on Windows."""
    exe_suffix = '.exe' if self.api.platform.is_win else ''
    return self.output_path / f'{binary_name}{exe_suffix}'

  def swarming_binary_path(self, binary_name: str) -> str:
    """Returns a relative path in the CAS archive to binary_name."""
    return f'./out/{BUILD_CONFIG}/{binary_name}'


def GetSwarmingDimensions(is_ci: bool) -> dict[str, str]:
  """Get a list of swarming dimensions to pass to swarming API."""
  dimensions = SWARMING_DIMENSIONS.copy()
  dimensions[POOL_DIMENSION] = FLEX_CI_POOL if is_ci else FLEX_TRY_POOL
  return dimensions


def GetHostToolLabel(platform: recipe_api.PlatformApi) -> str:
  """Determines what the platform label is, e.g. 'mac' or 'linux64'."""
  if platform.is_linux and platform.bits == 64:
    return 'linux64'
  raise ValueError('unknown or unsupported platform')  # pragma: no cover


def GenerateCoverageTestConstants(api: DEPS, paths: RepositoryPaths):
  """Generates fake file paths used for validation in code coverage tests."""
  if api.properties.get('is_valid_coverage_test', False):
    llvm_dir = (paths.checkout_path / 'third_party' / 'llvm-build' /
                'Release+Asserts' / 'bin')
    api.path.mock_add_paths(paths.checkout_path / 'build' / 'code_coverage')
    api.path.mock_add_paths(llvm_dir / 'llvm-profdata')
    api.path.mock_add_paths(llvm_dir / 'llvm-cov')
    api.path.mock_add_paths(paths.output_path / 'default.profraw')

  if api.properties.get('generate_test_profraw', False):
    api.path.mock_add_paths(paths.checkout_path / 'unit_tests.profraw')
    api.path.mock_add_paths(paths.checkout_path / 'e2e_tests.profraw')

  if api.properties.get('generate_test_profdata', False):
    api.path.mock_add_paths(paths.output_path / 'default.profdata')


def GetChangedFiles(api: DEPS, checkout_path: Path) -> list[str]:
  """Returns list of POSIX paths of files affected by patch."""
  files = []
  if api.tryserver.gerrit_change:
    patch_root = api.gclient.get_gerrit_patch_root()
    assert patch_root, ('local path is not configured for '
                        f'{api.tryserver.gerrit_change_repo_url}')
    with api.context(cwd=checkout_path):
      files = api.tryserver.get_files_affected_by_patch(patch_root)
    files = [api.path.relpath(str(path), checkout_path) for path in files]
  return files


def FormatGnArgs(properties: recipe_api.Properties) -> str:
  """Takes a list of properties and maps them to string gn arguments."""
  # Pass GN args as a list of strings.
  # Format: ["arg1=value1", "arg2=value2"]
  gn_args = dict(arg.split('=', 1) for arg in properties.get('gn_args', []))

  def format_arg(arg, value):
    if isinstance(value, str) and value.lower() not in ('true', 'false'):
      return f'{arg}="{value.strip(chr(34))}"'
    return f'{arg}={str(value).lower()}'

  return ' '.join(format_arg(k, v) for k, v in gn_args.items())


def UploadOpenscreenTestFilesToCas(api: DEPS, paths: RepositoryPaths) -> str:
  """Pushes files up to RBE-CAS server storage."""
  return api.cas.archive(
      'upload files to cas',
      paths.checkout_path,
      paths.unit_test_binary_path,
      paths.e2e_test_binary_path,
      paths.cast_e2e_test_script_path,
      paths.cast_sender_binary_path,
      paths.cast_receiver_binary_path,
      paths.test_data_path,
  )


class SwarmRequest:
  """A class to represent the data necessary to generate a swarming request."""

  def __init__(self, cas_digest: str, binary_path: str, task_name: str):
    self.cas_digest = cas_digest
    self.binary_path = binary_path
    self.task_name = task_name


def TriggerTest(api: DEPS, dimensions: dict[str, str],
                swarm_request: SwarmRequest):
  """Triggers a swarming test request."""
  request = api.swarming.task_request().with_name(swarm_request.task_name)
  task_slice = (request[0].with_command([
      swarm_request.binary_path
  ]).with_dimensions(**dimensions).with_cas_input_root(
      swarm_request.cas_digest))

  request = request.with_slice(0, task_slice)
  return api.swarming.trigger(f'trigger {swarm_request.task_name}',
                              requests=[request])


def SwarmTests(api: DEPS, paths: RepositoryPaths, dimensions: dict[str, str]):
  """Runs specific types of tests on a separate swarming bot."""
  cas_digest = UploadOpenscreenTestFilesToCas(api, paths)
  requests = {
      'unit tests':
      SwarmRequest(
          cas_digest,
          paths.swarming_binary_path(UNIT_TEST_BINARY_NAME),
          'unit tests',
      ),
      'e2e tests':
      SwarmRequest(cas_digest,
                   paths.swarming_binary_path(E2E_TEST_BINARY_NAME),
                   'e2e tests'),
  }

  if api.properties.get('cast_allow_developer_certificate'):
    requests['cast streaming e2e tests'] = SwarmRequest(
        cas_digest,
        f'./cast/{CAST_E2E_TEST_SCRIPT_NAME}',
        'cast streaming e2e tests',
    )

  # Trigger all tests in parallel.
  metadata = {
      name: TriggerTest(api, dimensions, req)[0]
      for name, req in requests.items()
  }

  # Collect all tests in parallel.
  output_dir = api.path.mkdtemp('swarming-output')
  results = api.swarming.collect(
      'collect swarming tests',
      list(metadata.values()),
      output_dir=output_dir,
      timeout='30m',
  )
  results_by_id = {r.id: r for r in results}

  # Check results.
  for name, meta in metadata.items():
    result = results_by_id[meta.id]
    if result.state in (
        api.swarming.TaskState.COMPLETED,
        api.swarming.TaskState.TIMED_OUT,
    ):
      if not result.success:
        step = api.step.empty(f'{name} failure')
        step.presentation.status = api.step.FAILURE
        raise api.step.StepFailure(f'{name} failure')
    else:
      result.analyze()


def SetCodeCoverageConstants(api: DEPS, checkout_path: Path, output_path: Path,
                             host_tool_label: str):
  """Configures the code_coverage and profiles modules."""
  llvm_dir = (checkout_path / 'third_party' / 'llvm-build' /
              'Release+Asserts' / 'bin')
  merge_libs_dir = checkout_path / 'build' / 'code_coverage'

  api.profiles.llvm_profdata_exec = llvm_dir / 'llvm-profdata'
  # Monkey-patching still needed for non-standard merge scripts location.
  api.profiles._merge_scripts_dir = merge_libs_dir

  api.code_coverage.build_dir = output_path
  # Use private members where public APIs are missing.
  api.code_coverage._platform = host_tool_label
  api.code_coverage._use_clang_coverage = True
  api.code_coverage._include_component_mapping = False

  missing = []
  if not api.path.exists(api.profiles.llvm_profdata_exec):
    missing.append(f'llvm-profdata at {api.profiles.llvm_profdata_exec}')
  if not api.path.exists(api.code_coverage.cov_executable):
    missing.append(f'llvm-cov at {api.code_coverage.cov_executable}')
  if not api.path.exists(merge_libs_dir):
    missing.append(f'merge scripts dir at {merge_libs_dir}')

  if missing:
    api.step.empty(
        'code coverage executable dependencies missing!',
        status=api.step.INFRA_FAILURE,
        step_text='\n'.join(missing),
    )


def GenerateAndUploadFullCoverageHtmlReport(
    api: DEPS,
    paths: RepositoryPaths,
    profdata_path: Path,
) -> None:
  """Generates and uploads an HTML coverage report for the full repository."""
  report_dir = api.code_coverage.report_dir
  cmd = [
      'python3',
      api.code_coverage.resource('make_report.py'),
      '--report-directory',
      report_dir,
      '--profdata-path',
      profdata_path,
      '--llvm-cov',
      api.code_coverage.cov_executable,
      '--compilation-directory',
      paths.output_path,
      '--binaries',
      paths.unit_test_binary_path,
      paths.e2e_test_binary_path,
  ]
  api.step('generate html report for full repo coverage', cmd)

  mimic_builder_name = api.code_coverage._compose_mimic_builder_name('overall')
  html_report_gs_path = api.code_coverage._compose_gs_path_for_coverage_data(
      data_type='html_report', mimic_builder_name=mimic_builder_name)
  upload_step = api.gsutil.upload(
      report_dir,
      api.code_coverage._gs_bucket,
      html_report_gs_path,
      link_name='html report',
      args=['-r'],
      multithreaded=True,
      name='upload html report',
  )
  upload_step.presentation.links['html report'] = (
      f'https://storage.cloud.google.com/{api.code_coverage._gs_bucket}/{html_report_gs_path}/index.html'
  )


def CalculateCodeCoverage(api: DEPS, paths: RepositoryPaths):
  """Calculates code coverage from raw coverage data."""
  temp_dir = api.profiles.profile_dir('profdata')

  # Process the raw code coverage data.
  api.step(
      'process raw coverage data',
      [
          'python3',
          api.profiles.merge_results_script,
          '--output-json',
          api.json.output(),
          '--task-output-dir',
          paths.checkout_path,
          '--profdata-dir',
          temp_dir,
          '--llvm-profdata',
          api.profiles.llvm_profdata_exec,
          '--build-dir',
          paths.output_path,
          '--chromium-src-dir',
          paths.checkout_path,
          '--per-cl-coverage',
      ],
  )

  source = paths.output_path / 'default.profdata'
  dest = temp_dir / 'default.profdata'

  # Workaround for merge_lib.py not respecting --profdata-dir correctly.
  if api.path.exists(source) and not api.path.exists(dest):
    api.file.copy('copy processed coverage data', source, dest)

  if api.path.exists(dest):
    api.step.empty('coverage data successfully processed')
  else:
    api.step.empty('failed to process coverage data', status=api.step.FAILURE)

  api.code_coverage.process_clang_coverage_data(
      binaries=[paths.unit_test_binary_path, paths.e2e_test_binary_path],
      upload_metadata=True,
  )

  if api.properties.get('is_ci', False) and api.path.exists(dest):
    overall_profdata = api.profiles.profile_dir().joinpath(
        'overall-merged.profdata')
    profdata_path = (overall_profdata
                     if api.path.exists(overall_profdata) else dest)
    GenerateAndUploadFullCoverageHtmlReport(api, paths, profdata_path)


def RunTestsLocally(api: DEPS, paths: RepositoryPaths,
                    build_targets: list[str]):
  """Runs unit tests and e2e tests locally."""
  if UNIT_TEST_BINARY_NAME in build_targets:
    api.step('run unit tests', [paths.unit_test_binary_path])
  if E2E_TEST_BINARY_NAME in build_targets:
    api.step('run e2e tests', [paths.e2e_test_binary_path])


def RunTestsAndCoverageLocally(api: DEPS, paths: RepositoryPaths):
  """Runs tests locally and calculates code coverage."""
  with api.step.nest('run tests'):
    with api.step.nest('perform pre-test cleanup'):
      files = api.file.glob_paths(
          'get files',
          paths.checkout_path,
          '**/*.profraw',
          test_data=[paths.output_path / 'default.profraw'],
      )
      for path in files:
        api.file.remove(f'remove {path}', path)

    # Run the Unit and E2E Tests.
    # We set LLVM_PROFILE_FILE to ensure we know exactly where the data goes.
    unit_test_profraw = paths.checkout_path / 'unit_tests.profraw'
    with api.context(env={'LLVM_PROFILE_FILE': str(unit_test_profraw)}):
      api.step('run unit tests', [paths.unit_test_binary_path])

    e2e_test_profraw = paths.checkout_path / 'e2e_tests.profraw'
    with api.context(env={'LLVM_PROFILE_FILE': str(e2e_test_profraw)}):
      api.step('run e2e tests', [paths.e2e_test_binary_path])

    if not api.path.exists(unit_test_profraw) and not api.path.exists(
        e2e_test_profraw):
      api.step.empty(
          'skip coverage calculations because no data was generated',
          status=api.step.FAILURE,
      )
    else:
      with api.step.nest('calculate code coverage'):
        CalculateCodeCoverage(api, paths)

  api.code_coverage._set_builder_output_properties_for_uploads()


def RunSteps(api: recipe_api.RecipeApi):
  """Main function body for execution on the current bot."""
  openscreen_config = api.gclient.make_config()
  solution = openscreen_config.solutions.add()
  solution.name = 'openscreen'
  solution.url = OPENSCREEN_REPO
  solution.deps_file = 'DEPS'

  if target_cpu := api.properties.get('target_cpu'):
    openscreen_config.target_cpu.add(target_cpu)
  api.gclient.c = openscreen_config

  # Explicitly set build_with_chromium to False to ensure 'build' is
  # checked out.
  solution.custom_vars['build_with_chromium'] = False

  update_result = api.bot_update.ensure_checkout()
  paths = RepositoryPaths(api, update_result.source_root.path)
  api.gclient.runhooks()

  is_ci = api.properties.get('is_ci', False)
  use_clang_coverage = api.properties.get('use_clang_coverage', False)
  build_targets = list(
      api.properties.get('build_targets', DEFAULT_BUILD_TARGETS))

  if use_clang_coverage:
    # Download coverage merge scripts from Chromium if they are missing.
    # The revision should match 'chrome_version' in openscreen/DEPS.
    chrome_version = '4a1c93eb7da3e438ea5cb677c783379a282ed75d'
    scripts_to_download = [
        'merge_results.py',
        'merge_steps.py',
        'merge_lib.py',
    ]
    with api.step.nest('download coverage scripts'):
      for script in scripts_to_download:
        api.step(
            f'download {script}',
            [
                'python3',
                update_result.source_root.path / 'tools' /
                'download-chromium-file.py',
                '--revision',
                chrome_version,
                '--path',
                f'testing/merge_scripts/code_coverage/{script}',
                '--output',
                update_result.source_root.path / 'build' / 'code_coverage' /
                script,
            ],
        )

  GenerateCoverageTestConstants(api, paths)

  # Initialize source directories for coverage and profiles modules early.
  api.profiles.source_dir = paths.checkout_path
  api.code_coverage.source_dir = paths.checkout_path

  env = {}
  if api.properties.get('is_asan'):
    env['ASAN_SYMBOLIZER_PATH'] = str(
        api.profiles.llvm_exec_path('llvm-symbolizer'))

  with api.context(cwd=paths.checkout_path, env=env):
    if use_clang_coverage:
      with api.step.nest('initialize code coverage') as coverage_step:
        try:
          host_tool_label = GetHostToolLabel(api.platform)
          SetCodeCoverageConstants(
              api,
              paths.checkout_path,
              paths.output_path,
              host_tool_label,
          )

          if not is_ci:
            changed_files = GetChangedFiles(api, paths.checkout_path)
            api.code_coverage.instrument(changed_files,
                                         output_dir=paths.output_path)
            api.step.empty(
                f'coverage calculations for {len(changed_files)} files',
                step_text='\n'.join(changed_files),
            )
        except Exception:  # pylint: disable=broad-except
          coverage_step.status = api.step.FAILURE
          use_clang_coverage = False

    # api.osx_sdk is a no-op on non-macOS platforms.
    with api.osx_sdk('mac'), api.depot_tools.on_path():
      gn_args = [
          'python3',
          api.depot_tools.gn_py_path,
          'gen',
          paths.output_path,
          '--check',
      ]
      if gn_args_str := FormatGnArgs(api.properties):
        gn_args.append(f'--args={gn_args_str}')

      api.step('gn gen', gn_args)

      ninja_cmd = [paths.ninja_path, '-C', paths.output_path]
      ninja_cmd.extend(build_targets)
      api.step('compile with ninja', ninja_cmd)

    # ARM64 tests are cross-compiled and run on swarming.
    if api.properties.get('target_cpu') == 'arm64' and not api.platform.is_mac:
      assert not use_clang_coverage, (
          'coverage is not supported on ARM64 builds.')
      SwarmTests(api, paths, GetSwarmingDimensions(is_ci))
    elif use_clang_coverage:
      RunTestsAndCoverageLocally(api, paths)
    else:
      RunTestsLocally(api, paths, build_targets)


def GenTests(api: recipe_api.RecipeTestApi):
  """Generates tests used to verify there are no python usage errors."""
  coverage_try_gn_args = [
      ('coverage_instrumentation_input_file='
       '"//.code-coverage/files_to_instrument.txt"'),
      'is_asan=true',
      'use_clang_coverage=true',
  ]

  yield api.test(
      'linux_x64',
      api.platform('linux', 64),
      api.buildbucket.try_build('openscreen', 'try'),
      api.properties(
          gn_args=coverage_try_gn_args,
          is_asan=True,
          use_clang_coverage=True,
          is_valid_coverage_test=True,
          generate_test_profraw=True,
          generate_test_profdata=True,
      ),
      api.step_data(
          'run tests.calculate code coverage.process raw coverage data',
          retcode=0,
      ),
  )
  yield api.test(
      'linux_x64_no_profdata_does_fail_bot',
      api.platform('linux', 64),
      api.buildbucket.try_build('openscreen', 'try'),
      api.properties(
          gn_args=coverage_try_gn_args,
          is_asan=True,
          use_clang_coverage=True,
          is_valid_coverage_test=True,
          generate_test_profraw=True,
      ),
      api.step_data(
          'run tests.calculate code coverage.process raw coverage data',
          retcode=0,
      ),
      api.expect_status('FAILURE'),
  )
  yield api.test(
      'linux_x64_no_profraw_does_fail_bot',
      api.platform('linux', 64),
      api.buildbucket.try_build('openscreen', 'try'),
      api.properties(
          gn_args=coverage_try_gn_args,
          is_asan=True,
          use_clang_coverage=True,
          is_valid_coverage_test=True,
      ),
      api.expect_status('FAILURE'),
  )
  yield api.test(
      'linux_x64_failed_coverage_init',
      api.platform('linux', 64),
      api.buildbucket.try_build('openscreen', 'try'),
      api.properties(gn_args=coverage_try_gn_args,
                     is_asan=True,
                     use_clang_coverage=True),
  )
  yield api.test(
      'linux_x64_ci',
      api.platform('linux', 64),
      api.buildbucket.ci_build('openscreen', 'ci'),
      api.properties(
          gn_args=['is_asan=true', 'use_clang_coverage=true'],
          is_asan=True,
          is_ci=True,
          use_clang_coverage=True,
          is_valid_coverage_test=True,
          generate_test_profraw=True,
          generate_test_profdata=True,
      ),
      api.step_data(
          'run tests.calculate code coverage.process raw coverage data',
          retcode=0,
      ),
  )
  yield api.test(
      'linux_x64_tsan_rel',
      api.platform('linux', 64),
      api.buildbucket.try_build('openscreen', 'try'),
      api.properties(gn_args=['is_debug=false', 'is_tsan=true']),
  )
  yield api.test(
      'linux_x64_msan_rel',
      api.platform('linux', 64),
      api.buildbucket.try_build('openscreen', 'try'),
      api.properties(gn_args=['is_debug=false', 'is_msan=true']),
  )
  yield api.test(
      'linux_arm64',
      api.platform('linux', 64),
      api.buildbucket.try_build('openscreen', 'try'),
      api.properties(
          target_cpu='arm64',
          gn_args=['is_component_build=false', 'target_cpu=arm64'],
      ),
  )
  yield api.test(
      'linux_arm64_ci',
      api.platform('linux', 64),
      api.buildbucket.try_build('openscreen', 'ci'),
      api.properties(
          target_cpu='arm64',
          is_ci=True,
          gn_args=['is_component_build=false', 'target_cpu=arm64'],
      ),
  )
  yield api.test(
      'linux_arm64_cast_receiver',
      api.platform('linux', 64),
      api.buildbucket.try_build('openscreen', 'try'),
      api.properties(
          target_cpu='arm64',
          cast_allow_developer_certificate=True,
          gn_args=[
              'cast_allow_developer_certificate=true',
              'have_ffmpeg=true',
              'have_libopus=true',
              'have_libsdl2=true',
              'have_libvpx=true',
              'is_component_build=false',
              'target_cpu=arm64',
          ],
      ),
  )

  failed_result = api.swarming.task_result(
      id='0',
      name=UNIT_TEST_BINARY_NAME,
      state=api.swarming.TaskState.COMPLETED,
      failure=True,
  )
  yield api.test(
      'linux_arm64_with_collect_COMPLETED_and_failed',
      api.platform('linux', 64),
      api.buildbucket.try_build('openscreen', 'try'),
      api.properties(
          target_cpu='arm64',
          gn_args=['is_component_build=false', 'target_cpu=arm64'],
      ),
      api.override_step_data('collect swarming tests',
                             api.swarming.collect([failed_result])),
      api.expect_status('FAILURE'),
  )

  timeout_result = api.swarming.task_result(
      id='0',
      name=UNIT_TEST_BINARY_NAME,
      state=api.swarming.TaskState.TIMED_OUT)
  yield api.test(
      'linux_arm64_with_collect_TIMED_OUT',
      api.platform('linux', 64),
      api.buildbucket.try_build('openscreen', 'try'),
      api.properties(
          target_cpu='arm64',
          gn_args=['is_component_build=false', 'target_cpu=arm64'],
      ),
      api.override_step_data('collect swarming tests',
                             api.swarming.collect([timeout_result])),
      api.expect_status('FAILURE'),
  )

  died_result = api.swarming.task_result(id='0',
                                         name=UNIT_TEST_BINARY_NAME,
                                         state=api.swarming.TaskState.BOT_DIED)
  yield api.test(
      'linux_arm64_with_collect_BOT_DIED',
      api.platform('linux', 64),
      api.buildbucket.try_build('openscreen', 'try'),
      api.properties(
          target_cpu='arm64',
          gn_args=['is_component_build=false', 'target_cpu=arm64'],
      ),
      api.override_step_data('collect swarming tests',
                             api.swarming.collect([died_result])),
      api.expect_status('INFRA_FAILURE'),
  )
  yield api.test(
      'mac_arm64',
      api.platform('mac', 64),
      api.buildbucket.try_build('openscreen', 'try'),
      api.properties(target_cpu='arm64', gn_args=['target_cpu=arm64']),
  )
  yield api.test(
      'win_x64',
      api.platform('win', 64),
      api.buildbucket.try_build('openscreen', 'try'),
      api.properties(build_targets=['gn_all']),
  )
  yield api.test(
      'win_x64_ci',
      api.platform('win', 64),
      api.buildbucket.ci_build('openscreen', 'ci'),
      api.properties(build_targets=['gn_all']),
  )
