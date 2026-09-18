# FoodLoop Companion QuickApp（伴侣端骨架）

FoodLoop 的手机/手表端伴侣体验（M4 里程碑的前置原型）。当前为 UI 骨架 + mock 数据，展示库存审阅与到期状态，后续接入板端 `records.jsonl` 数据源。

## 页面

`src/pages/index/index.ux`

- 状态统计：USE TODAY / USE SOON / EXPIRED / TOTAL 四格
- 库存列表：名称、储存方式、到期日、状态徽标（EXPIRED / TODAY / SOON / OK / VERIFY）
- `needs_confirmation` 的条目显示 VERIFY，不参与提醒统计（与板端 M2/M3 隐私边界一致）
- 空库存提示

## 数据契约

mock 数据字段与板端 `/mnt/foodloop/records.jsonl` 的 `draft.items` 一致：

```json
{
  "name": "MILK",
  "category": "dairy",
  "expiry_date": "2026-08-10",
  "storage": "refrigerated",
  "needs_confirmation": false,
  "confidence": 0.9
}
```

日期比较使用与 `app/foodloop/foodloop_main.c` 中 `foodloop_days_from_civil`
等价的算法（Howard Hinnant civil-from-days），保证伴侣端与板端状态一致。

## 后续集成

- [ ] 通过局域网网关（8789）或板端同步接口拉取真实 `records.jsonl`
- [ ] 确认/删除记录操作与板端删除流程联动
- [ ] 每日提醒与 ai_agent cron 注册联动
