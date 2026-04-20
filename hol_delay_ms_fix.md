# delay_weight 구현 정리

## 개요
`delay_weight`는 Packet Delay Budget (PDB)과 Head of Line (HOL) delay를 기반으로 계산되는 스케줄링 가중치입니다. 이 값은 `final_priority` 계산에 사용되어 지연이 중요한 트래픽에 더 높은 우선순위를 부여합니다.

## 문제점 (Before)

### 상황
기존에는 RLC에서 MAC으로 `hol_toa`가 전달되지 않았습니다. MAC → Scheduler 경로는 이미 구현되어 있었지만, RLC에서 `hol_toa`를 전달하지 않아서 `mac_dl_buffer_state_indication_message.hol_toa`가 항상 `nullopt`였습니다.

### 결과
- `srsran_scheduler_adapter::handle_dl_buffer_state_update()` 함수의 `hol_toa` 변환 로직(`if (mac_dl_bs_ind.hol_toa.has_value())`)이 실행되지 않음
- 스케줄러의 `dl_logical_channel_manager`에 전달되는 `hol_toa`가 항상 빈 값(`slot_point{}`)
- `u.dl_hol_toa(lc->lcid)`가 항상 유효하지 않은 값 반환
- `delay_weight` 계산 시 `hol_toa.valid() and slot_tx >= hol_toa` 조건이 항상 false
- **결과적으로 `delay_weight`가 항상 1.0으로 계산되어 PDB 기반 지연 우선순위 스케줄링이 동작하지 않음**

## 해결책

### 변경 내용
RLC → MAC으로 `hol_toa`를 전달하는 경로만 추가했습니다.

### 동작 원리
1. **RLC 레이어**: `hol_toa`를 `steady_clock::time_point`에서 `system_clock::time_point`로 변환하여 `rlc_buffer_state`에 포함
2. **MAC 레이어**: RLC에서 받은 `hol_toa`를 `mac_dl_buffer_state_indication_message`에 포함하여 스케줄러로 전달
3. **Scheduler Adapter (기존 코드)**: `system_clock::time_point`를 `slot_point`로 변환하는 로직이 이미 존재했으나, `hol_toa`가 비어있어 실행되지 않았음 → 이제 실행됨
4. **Scheduler UE Context (기존 코드)**: `hol_toa`를 저장하고 조회하는 로직이 이미 존재했으나, 항상 빈 값이었음 → 이제 실제 값이 저장됨

### 결과
RLC → MAC 경로만 추가하니 자연스럽게 전체 경로(RLC → MAC → Scheduler)가 완성되어 `delay_weight`가 정상적으로 계산되기 시작했습니다.

## hol_toa 전달 경로 추가

**참고**: MAC → Scheduler 경로는 이미 구현되어 있었습니다. RLC → MAC 경로만 추가하여 전체 경로를 완성했습니다.

### 1. RLC 레이어: `hol_toa`를 `system_clock::time_point`로 변환 (추가됨)

**파일**: `include/srsran/rlc/rlc_buffer_state.h`, `lib/rlc/rlc_tx_am_entity.cpp`, `lib/rlc/rlc_tx_um_entity.cpp`, `lib/rlc/rlc_tx_tm_entity.cpp`

**변경 내용**:
- `rlc_buffer_state` 구조체의 `hol_toa` 타입을 `std::chrono::steady_clock::time_point`에서 `std::chrono::system_clock::time_point`로 변경
- RLC의 `get_buffer_state()` 함수에서 `steady_clock`의 `hol_toa`를 `system_clock`으로 변환:
  ```cpp
  if (steady_hol_toa.has_value()) {
    auto now_steady = std::chrono::steady_clock::now();
    auto delay = now_steady - steady_hol_toa.value();
    bs.hol_toa = std::chrono::system_clock::now() - delay;
  }
  ```

**이유**: `steady_clock`과 `system_clock`의 epoch가 다르기 때문에, 상위 레이어에서 안전하게 사용하기 위해 `system_clock`으로 변환

---

### 2. MAC 레이어: RLC에서 받은 `hol_toa`를 스케줄러로 전달 (추가됨)

**파일**: `include/srsran/mac/mac_ue_control_information_handler.h`, `lib/mac/mac_dl/mac_cell_processor.cpp`

**변경 내용**:
- `mac_dl_buffer_state_indication_message` 구조체에 `hol_toa` 필드 추가:
  ```cpp
  struct mac_dl_buffer_state_indication_message {
    // ... 기존 필드들 ...
    std::optional<std::chrono::system_clock::time_point> hol_toa;  // 추가됨
  };
  ```

- `mac_cell_processor.cpp`에서 RLC의 `hol_toa`를 MAC 메시지에 포함:
  ```cpp
  rlc_buffer_state rlc_bs = bearer->on_buffer_state_update();
  mac_dl_buffer_state_indication_message bs{
      ue_mng.get_ue_index(grant.pdsch_cfg.rnti), lc_info.lcid.to_lcid(), rlc_bs.pending_bytes};
  if (rlc_bs.hol_toa.has_value()) {
    bs.hol_toa = rlc_bs.hol_toa.value();  // RLC에서 받은 hol_toa 전달 (추가됨)
  }
  sched.handle_dl_buffer_state_update(bs);  // 기존 경로 사용
  ```

**참고**: `sched.handle_dl_buffer_state_update(bs)` 호출은 이미 존재했으며, `hol_toa` 필드만 추가하여 전달하도록 수정했습니다.

---

### 3. Scheduler Adapter: `hol_toa`를 `slot_point`로 변환 (기존 코드)

**파일**: `lib/mac/mac_sched/srsran_scheduler_adapter.cpp`

**상태**: 이미 구현되어 있었음

**기능**:
- `dl_buffer_state_indication_message` 구조체에 `hol_toa` 필드가 이미 존재 (`slot_point` 타입)
- `handle_dl_buffer_state_update()` 함수에서 `system_clock::time_point`를 `slot_point`로 변환하는 로직이 이미 구현되어 있었음:
  ```cpp
  if (mac_dl_bs_ind.hol_toa.has_value()) {
    const high_resolution_clock::time_point sl_tp = last_slot_tp.load(std::memory_order_relaxed);
    if (sl_tp != high_resolution_clock::time_point{}) {
      auto system_toa = mac_dl_bs_ind.hol_toa.value();
      auto system_now = system_clock::now();
      auto delay = system_now - system_toa;  // 상대 지연 계산
      auto hr_toa = sl_tp - std::chrono::duration_cast<high_resolution_clock::duration>(delay);
      bs.hol_toa = chrono_to_slot_point(hr_toa, sl_tp, last_slot_point.load(std::memory_order_relaxed));
    }
  }
  ```

**참고**: RLC → MAC 경로가 추가되기 전에는 `mac_dl_bs_ind.hol_toa`가 항상 `nullopt`였기 때문에 이 변환 로직이 실행되지 않았습니다.

---

### 4. Scheduler UE Context: `hol_toa` 저장 및 조회 (기존 코드)

**파일**: `lib/scheduler/ue_context/dl_logical_channel_manager.h`, `lib/scheduler/ue_context/ue.cpp`

**상태**: 이미 구현되어 있었음

**기능**:
- `dl_logical_channel_manager`의 `channel_context` 구조체에 `hol_toa` 필드가 이미 존재
- `handle_dl_buffer_status_indication()` 함수에서 `hol_toa`를 저장하는 로직이 이미 구현되어 있었음:
  ```cpp
  void handle_dl_buffer_status_indication(lcid_t lcid, unsigned buffer_status, slot_point hol_toa = {})
  {
    channels[lcid].buf_st  = std::min(buffer_status, max_buffer_status);
    channels[lcid].hol_toa = hol_toa;  // hol_toa 저장 (기존 코드)
  }
  ```

- `hol_toa()` getter 함수가 이미 존재:
  ```cpp
  slot_point hol_toa(lcid_t lcid) const { return is_active(lcid) ? channels[lcid].hol_toa : slot_point{}; }
  ```

- `ue.cpp`에서 `hol_toa`를 `dl_logical_channel_manager`에 전달하는 로직이 이미 구현되어 있었음:
  ```cpp
  void ue::handle_dl_buffer_state_indication(lcid_t lcid, unsigned bs, slot_point hol_toa = {})
  {
    dl_lc_ch_mgr.handle_dl_buffer_status_indication(lcid, pending_bytes, hol_toa);  // 기존 코드
  }
  ```

**참고**: RLC → MAC 경로가 추가되기 전에는 `hol_toa`가 항상 빈 값(`slot_point{}`)으로 전달되었습니다.

---

## 변경된 파일

### `lib/scheduler/policy/scheduler_time_qos.cpp`

#### 1. `combine_qos_metrics` 함수에 `delay_weight` 파라미터 추가

**위치**: 131-145줄

```cpp
static double combine_qos_metrics(double                           pf_weight,
                                  double                           gbr_weight,
                                  double                           prio_weight,
                                  double                           delay_weight,  // 추가됨
                                  const time_qos_scheduler_config& policy_params)
{
  // ... 기존 코드 ...
  
  // The return is a combination of QoS priority, ARP priority, GBR and PF weight functions.
  return gbr_weight * pf_weight * prio_weight * delay_weight;  // delay_weight 곱셈 추가
}
```

**변경 내용**:
- `delay_weight` 파라미터 추가
- 반환값 계산에 `delay_weight` 곱셈 추가

---

#### 2. `compute_dl_qos_weights` 함수에 `delay_weight` 계산 로직 추가

**위치**: 162줄 (변수 선언), 192-225줄 (계산 로직), 247-261줄 (최종 처리)

##### 2-1. `delay_weight` 변수 선언

```cpp
double delay_weight = 0;  // 162줄
```

##### 2-2. `delay_weight` 계산 로직

```cpp
// 192-225줄
slot_point hol_toa = u.dl_hol_toa(lc->lcid);
if (hol_toa.valid() and slot_tx >= hol_toa) {
  const unsigned hol_delay_ms = (slot_tx - hol_toa) / slot_tx.nof_slots_per_subframe();
  const unsigned pdb          = lc->qos->runtime_qos.packet_delay_budget_ms;
  double delay_contrib = hol_delay_ms / static_cast<double>(pdb);
  delay_weight += delay_contrib;
  
  // Log delay_weight calculation details (periodically)
  static unsigned delay_log_counter = 0;
  if ((delay_log_counter++ % 100) == 0) {
    static srslog::basic_logger& logger = srslog::fetch_basic_logger("SCHED");
    logger.info("[DELAY-WEIGHT] UE{} LCID{} hol_toa={} slot_tx={} hol_delay_ms={} PDB={}ms delay_contrib={:.3f} delay_weight={:.3f}",
                u.ue_index(),
                static_cast<unsigned>(lc->lcid),
                hol_toa.to_uint(),
                slot_tx.to_uint(),
                hol_delay_ms,
                pdb,
                delay_contrib,
                delay_weight);
  }
} else {
  // Log when hol_toa is invalid or condition not met (periodically)
  static unsigned hol_toa_log_counter = 0;
  if ((hol_toa_log_counter++ % 100) == 0) {
    static srslog::basic_logger& logger = srslog::fetch_basic_logger("SCHED");
    logger.info("[DELAY-WEIGHT] UE{} LCID{} hol_toa_valid={} hol_toa={} slot_tx={} (condition not met, delay_weight not updated)",
                u.ue_index(),
                static_cast<unsigned>(lc->lcid),
                hol_toa.valid(),
                hol_toa.valid() ? hol_toa.to_uint() : 0,
                slot_tx.to_uint());
  }
}
```

**계산 방식**:
- `hol_delay_ms = (slot_tx - hol_toa) / slot_tx.nof_slots_per_subframe()`: HOL delay를 밀리초로 계산
- `delay_contrib = hol_delay_ms / pdb`: HOL delay를 PDB로 나눈 비율
- `delay_weight += delay_contrib`: 각 LC의 기여도를 누적

##### 2-3. `delay_weight` 최종 처리 및 로깅

```cpp
// 247-261줄
double delay_weight_before = delay_weight;
delay_weight = policy_params.pdb_enabled and delay_weight != 0 ? delay_weight : 1.0;

// Log delay_weight final value and reason (periodically)
static unsigned delay_final_log_counter = 0;
if ((delay_final_log_counter++ % 100) == 0) {
  static srslog::basic_logger& logger = srslog::fetch_basic_logger("SCHED");
  logger.info("[DELAY-WEIGHT-FINAL] UE{} delay_weight_before={:.3f} pdb_enabled={} delay_weight_after={:.3f} (reason: {})",
              u.ue_index(),
              delay_weight_before,
              policy_params.pdb_enabled,
              delay_weight,
              (policy_params.pdb_enabled and delay_weight_before != 0) ? "calculated" : 
              (not policy_params.pdb_enabled) ? "pdb_disabled" : "delay_weight_was_zero");
}
```

**처리 로직**:
- `pdb_enabled`가 false이거나 `delay_weight`가 0이면 1.0으로 설정
- 그 외의 경우 계산된 `delay_weight` 값 사용

---

#### 3. `policy_params.pdb_enabled` 조건 추가

**위치**: 163줄

```cpp
if (policy_params.gbr_enabled or policy_params.priority_enabled or policy_params.pdb_enabled) {
  // delay_weight 계산 루프
}
```

**변경 내용**:
- `policy_params.pdb_enabled` 조건 추가하여 PDB 기능 활성화 시에만 `delay_weight` 계산

---

#### 4. `final_priority` 계산에 `delay_weight` 사용

**위치**: 320줄

```cpp
double final_priority = combine_qos_metrics(pf_weight, gbr_weight, prio_weight, delay_weight, policy_params);
```

**변경 내용**:
- `combine_qos_metrics` 호출 시 `delay_weight` 파라미터 전달

---

#### 5. 로그에 `delay_weight` 출력 추가

**위치**: 286줄, 292줄

```cpp
// 286줄
logger.info("DL Priority calc: UE{} min_combined_prio={}, prio_weight={:.3f}, pf_weight={:.3f}, gbr_weight={:.3f}, delay_weight={:.3f}",
            u.ue_index(),
            min_combined_prio,
            prio_weight,
            pf_weight,
            gbr_weight,
            delay_weight);  // delay_weight 로그 출력 추가
```

**변경 내용**:
- DL Priority 계산 로그에 `delay_weight` 값 추가

---

## 계산 공식

### delay_weight 계산
```
delay_weight = Σ (hol_delay_ms / PDB)
```

여기서:
- `hol_delay_ms = (slot_tx - hol_toa) / slots_per_subframe`: Head of Line delay (밀리초)
- `PDB = packet_delay_budget_ms`: Packet Delay Budget (밀리초)
- 각 LC(Logical Channel)에 대해 계산된 `delay_contrib`를 누적

### final_priority 계산
```
final_priority = gbr_weight × pf_weight × prio_weight × delay_weight
```

---

## 동작 방식

1. **HOL TOA 조회**: `u.dl_hol_toa(lc->lcid)`로 각 LC의 Head of Line Time of Arrival 조회
2. **조건 확인**: `hol_toa.valid() and slot_tx >= hol_toa` 조건 확인
3. **HOL Delay 계산**: `(slot_tx - hol_toa) / slots_per_subframe`로 밀리초 단위 delay 계산
4. **Delay Contribution 계산**: `hol_delay_ms / PDB`로 정규화된 delay 기여도 계산
5. **누적**: 각 LC의 `delay_contrib`를 `delay_weight`에 누적
6. **최종 처리**: `pdb_enabled` 및 `delay_weight` 값에 따라 1.0 또는 계산값 사용
7. **final_priority 계산**: `combine_qos_metrics`에서 다른 가중치들과 곱셈

---

## 로깅

### 1. `[DELAY-WEIGHT]` 로그
- **주기**: 100번마다
- **내용**: 각 LC별 HOL delay, PDB, delay_contrib, 누적 delay_weight
- **조건**: `hol_toa.valid() and slot_tx >= hol_toa`가 true일 때

### 2. `[DELAY-WEIGHT]` 로그 (조건 불만족)
- **주기**: 100번마다
- **내용**: hol_toa가 유효하지 않거나 조건을 만족하지 않을 때
- **조건**: `hol_toa.valid() and slot_tx >= hol_toa`가 false일 때

### 3. `[DELAY-WEIGHT-FINAL]` 로그
- **주기**: 100번마다
- **내용**: 최종 delay_weight 값과 계산 이유 (calculated/pdb_disabled/delay_weight_was_zero)

### 4. DL Priority calc 로그
- **주기**: 100번마다
- **내용**: 모든 가중치 값 (prio_weight, pf_weight, gbr_weight, delay_weight)

---

## 참고사항

- **UL (Uplink)**: 현재 UL에는 `delay_weight` 계산이 구현되지 않았으며, `combine_qos_metrics` 호출 시 `delay_weight`로 1.0을 전달합니다 (391줄).
- **HOL TOA**: `hol_toa`는 RLC 레이어에서 `system_clock::time_point`로 변환되어 전달되며, 스케줄러에서 `slot_point`로 변환됩니다.
- **PDB**: `packet_delay_budget_ms`는 5QI 기반 QoS 특성에서 가져오며, DSCP 기반 5QI 매핑을 통해 동적으로 업데이트될 수 있습니다.

