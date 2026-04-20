# PDB / hol_delay_ms 스케줄러 반영 검증

DSCP에 설정한 PDB 값이 스케줄러에 잘 들어가고, `hol_delay_ms`가 그 PDB 안팎으로 계산되는지 정리한 문서입니다.

---

## 1. 결론 요약

- **PDB는 DSCP → 5QI → 표준 5QI 특성(PDB) 경로로 스케줄러에 반영됩니다.**
- **hol_delay_ms는 `runtime_qos.packet_delay_budget_ms`(위에서 설정된 PDB)와 같은 단위(ms)로 비교·사용됩니다.**
- `pdb_enabled` 기본값은 `true`라서, 별도로 끄지 않으면 PDB 기반 delay_weight가 적용됩니다.

---

## 2. PDB가 스케줄러에 들어가는 경로

### 2.1 호출 순서 (DL 우선순위 계산 시)

`lib/scheduler/policy/scheduler_time_qos.cpp`의 `ue_ctxt::compute_dl_prio()`:

1. `compute_dl_avg_rate(u, nof_slots_elapsed);`
2. **`apply_5qi_based_runtime_overrides(u);`** ← 여기서 DSCP 기반으로 **runtime_qos.packet_delay_budget_ms(PDB)** 갱신
3. `compute_dl_qos_weights(u, ..., pdcch_slot, parent->params);` ← 여기서 **갱신된 PDB** 사용

따라서 **스케줄링 가중치를 계산할 때 쓰는 PDB는 항상 “현재 DSCP에 맞는 PDB”**입니다.

### 2.2 apply_5qi_based_runtime_overrides() 내부 (요약)

- `dscp_qos_mapper::get_instance().get_dscp_for_ue(ue_index)` 로 UE의 현재 DSCP 조회.
- `map_dscp_to_5qi(dscp)` 또는 `map_dscp_to_5qi_using_standard_mapping(dscp)` 로 **effective_5qi** 결정.
- `get_5qi_to_qos_characteristics_mapping(effective_5qi)` 로 표준 5QI 특성 조회.
- 여기서 **`packet_delay_budget_ms`** 를 가져와  
  `lc->qos->runtime_qos.packet_delay_budget_ms = effective_pdb` 로 설정.

즉, **설정한 DSCP → 5QI → 해당 5QI의 PDB**가 그대로 스케줄러의 `runtime_qos.packet_delay_budget_ms`에 들어갑니다.

---

## 3. hol_delay_ms 계산 및 PDB와의 관계

### 3.1 계산 위치 및 수식

`compute_dl_qos_weights()` 안 (동일 파일):

```cpp
slot_point hol_toa = u.dl_hol_toa(lc->lcid);
if (hol_toa.valid() and slot_tx >= hol_toa) {
  const unsigned hol_delay_ms = (slot_tx - hol_toa) / slot_tx.nof_slots_per_subframe();
  const unsigned pdb          = lc->qos->runtime_qos.packet_delay_budget_ms;
  double delay_contrib = hol_delay_ms / static_cast<double>(pdb);
  delay_weight += delay_contrib;
  // [DELAY-WEIGHT] 로그: hol_delay_ms, PDB, delay_contrib 출력
}
```

- **hol_delay_ms**: (현재 슬롯 − HOL 도착 슬롯) / 슬롯 per subframe → **단위는 ms(서브프레임 단위)**.
- **pdb**: 위에서 설정한 **DSCP에 해당하는 PDB(ms)**.
- **delay_contrib**: hol_delay_ms / PDB → PDB 대비 지연 비율.  
  hol_delay_ms가 PDB 안이면 1 미만, 넘으면 1 초과.

즉, **설정한 DSCP의 PDB 값 “안팎”은 delay_contrib가 1 근처(안이면 &lt;1, 밖이면 &gt;1)로 나오도록 되어 있습니다.

### 3.2 hol_toa 전달 여부

- `hol_toa`가 RLC → MAC → 스케줄러로 전달되어야 위 블록이 실행됩니다.
- 전달이 되지 않으면 `hol_toa.valid()`가 false여서 **delay_weight는 1.0**으로만 쓰입니다.
- RLC→MAC→스케줄러 경로는 `hol_delay_ms_fix.md`에 정리되어 있으므로, 해당 수정이 적용된 빌드인지 확인하면 됩니다.

---

## 4. 설정한 DSCP별 PDB (현재 매핑 기준)

`include/srsran/sdap/dscp_qos_mapper.h`의 DSCP→5QI 테이블과  
`lib/ran/qos/five_qi_qos_mapping.cpp`의 5QI→PDB를 조합하면:

| DSCP | 5QI | PDB (ms) |
|------|-----|----------|
| 0    | 9   | 300      |
| 15   | 84  | 30       |
| 24   | 80  | 10       |
| 44   | 66  | 100      |

iperf3 시나리오(0→44→24→15)에서:

- **hol_delay_ms가 각 구간의 PDB(300 / 100 / 10 / 30 ms) 안팎**으로 나오면,  
  DSCP에 따른 PDB가 스케줄러에 잘 반영된 것으로 보면 됩니다.

---

## 5. 로그로 확인하는 방법

- **스케줄러에 쓰이는 PDB**
  - `[STEP6-SCHED]` / `[SCHED-QoS]`: `PDB=...ms` 에서 **현재 LC에 적용된 PDB** 확인.
- **hol_delay vs PDB**
  - `[DELAY-WEIGHT]`: `hol_delay_ms=... PDB=...ms delay_contrib=...`  
    → hol_delay_ms가 PDB 대비 얼마나 되는지(안팎) 확인.

예시:

```text
[DELAY-WEIGHT] UE0 LCID3 hol_toa=... slot_tx=... hol_delay_ms=8 PDB=10ms delay_contrib=0.800 delay_weight=...
```

→ PDB 10ms 구간에서 hol_delay_ms 8ms면 PDB “안”으로 나오는 것이고, 설정이 의도대로 반영된 것입니다.

---

## 6. 정리

- **PDB**: DSCP → 5QI → `get_5qi_to_qos_characteristics_mapping()` → `runtime_qos.packet_delay_budget_ms` 로 스케줄러에 들어가며,  
  `compute_dl_prio()`에서 **apply_5qi_based_runtime_overrides()가 먼저 호출**되므로 **항상 최신 DSCP에 맞는 PDB**가 사용됩니다.
- **hol_delay_ms**: 위와 같은 단위(ms)의 PDB와 함께 `delay_contrib = hol_delay_ms / PDB`로 쓰이므로,  
  설정한 DSCP의 PDB 값에 따라 hol_delay_ms가 그 값의 “안팎”으로 나오는 구조가 맞게 되어 있습니다.
- **실제로 “안팎”으로 나오는지**는 `[DELAY-WEIGHT]` 로그에서 hol_delay_ms와 PDB를 같이 보면 됩니다.
