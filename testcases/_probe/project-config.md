```yaml
project: DeskTidy
env_prefix: DESKTIDY
execution_driver: code
execution_tool: pytest
frontend_dir: native
module_map: testcases/_probe/module-map.md
golden_dir: assets/
testcases_dir: testcases/
auth: none
env_gate: local-direct
local_defaults:
  base_url: ""
  app_path: ""
  fallback_urls: []
```
