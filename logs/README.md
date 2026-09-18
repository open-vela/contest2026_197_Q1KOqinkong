# logs/ — AI Coding 日志目录

存放你在开发中与 AI 工具的对话日志，和作品代码一并提交。

若真实日志不可恢复，可保留一份明确标注为非日志的缺失说明；该说明不替代
官方要求的 JSONL 会话导出。

> 当前目录中的模板日志已经移除。若没有真实导出，请保留 LOGS_UNAVAILABLE.md；它是缺失说明，不是 JSONL 会话日志。

当前已从本机 Codex Desktop 会话恢复一份真实的项目开发摘录。它位于
`logs/Q1KO-Official/`，并在 manifest 中标注为 `partial excerpt`；它不是完整
会话导出。`docs/supporting-evidence/codex/` 中保留的是带临时身份的预览和
转换来源说明。

## 目录结构

```text
logs/
└── <github_login>/              # 你的 GitHub 用户名，一人一目录
    ├── manifest.json            # 会话清单
    └── <date>/                  # 日期 YYYY-MM-DD
        └── <tool>__<sid>.jsonl  # 一个会话一个文件（工具名与 session id 用 __ 连接）
```

- `<tool>`：`claude-code` / `opencode` / `codex` / `kiro`
- 每个 `.jsonl` 每行一个事件，由组委会提供的日志归集工具导出，**只提交 JSONL 本身**。

导出与提交的完整步骤、字段定义见[《AI Coding 日志归集与提交手册》](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/ai_coding_log_guide.md)。
