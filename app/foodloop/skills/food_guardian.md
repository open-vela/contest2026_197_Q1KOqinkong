# Food Guardian

FoodLoop 的食物守护 Skill：基于设备上的已确认食物记录，给出保质期状态、到期提醒和"先用哪个"建议。只使用确认过的记录，歧义日期绝不产生提醒。

## When to use

用户问以下内容时使用本 Skill：
- "家里有什么快过期了？" / "牛奶还能喝吗" / "今天先吃什么"
- "帮我记一下这袋面包的保质期" / "扫描结果怎么样"
- "帮我提醒我 X 天后处理冰箱里的剩菜"
- 任何关于食品库存、保质期、过期、购物清单的问题

## How to use

1. **读取本地食物记录**（设备上运行 NSH 命令）：
   - `foodloop list` — 列出 `/mnt/foodloop/records.jsonl`（或 `/data/foodloop-records.jsonl` 回退）里的全部记录，含名称与到期日。
   - `foodloop status [YYYY-MM-DD]` — 按参考日期输出每条记录的 `USE TODAY` / `USE SOON` / `EXPIRED` / `OK`。不带参数时用系统时钟；测试或时钟未同步时可显式传日期。

2. **区分确认状态（隐私与数据完整性边界）**：
   - 每条记录 JSON：`{"confirmed":true,"ts":...,"draft":{"items":[{...}],"scan_notes":"..."}}`
   - 只对 `confirmed == true` 且 `needs_confirmation == false` 的条目生成提醒。
   - `needs_confirmation == true` 表示日期/数量有歧义，**必须**先请用户确认，不得据此提醒。
   - 到期日字段 `expiry_date` 必须是 `YYYY-MM-DD` 或 `null`；`null` 时不得臆造日期。

3. **生成提醒（用 cron 工具）**：
   - 单次提醒：`cron_add {"name":"foodloop-<item>","schedule_type":"at","at_epoch":<到期前一天或当天0点>,"message":"<名称> 将于 <日期> 到期，建议优先使用"}`。
   - 周期性检查：`cron_add {"name":"foodloop-daily","schedule_type":"every","interval_s":86400,"message":"检查食物保质期状态，调用 foodloop status 并汇报 USE TODAY / USE SOON / EXPIRED 条目"}`。
   - 提醒消息里引用真实记录与到期日，不要编造数量或营养信息。

4. **回答"先吃什么"**：优先推荐 `USE TODAY` > `USE SOON` > 有效期较近的 `OK` 条目；说明理由（哪条记录、哪天到期、什么储存方式）。

5. **新扫描结果确认**：设备上 BOOT 确认后记录才落盘。Agent 若被问到刚扫描的结果，应引导查看 `foodloop list` 或确认流，而不是把草稿当作已确认库存。

## Rules

- 过期判断只依据 `expiry_date`，绝不从 `date_type=production` 的生产日期推算到期日。
- `needs_confirmation=true` 的条目不提醒、不计数、不推荐。
- 不宣称没有记录支持的"营养/热量"数据。
- 记录删除、导出等操作需用户明确要求后执行，并说明影响（删除记录同时移除其提醒）。

## Example

用户："冰箱里有什么快过期了？"

1. 运行 `foodloop status`（或 `foodloop status 2026-08-15`）。
2. 输出示例：
   ```
   FoodLoop: status today 2026-08-15:
     MILK                EXPIRED (2026-08-10)
     YOGURT              USE TODAY (2026-08-15)
     BREAD               USE SOON (2026-08-18)
     RICE                OK (2027-01-01)
   ```
3. 回复："牛奶 8/10 已过期建议处理，酸奶今天到期先喝，面包 8/18 前吃完。需要我设置提醒吗？"

用户："提醒我每天检查一次。"

→ `cron_add {"name":"foodloop-daily","schedule_type":"every","interval_s":86400,"message":"调用 foodloop status 汇报食物保质期状态"}`
→ "好的，已设置每日检查提醒。"
