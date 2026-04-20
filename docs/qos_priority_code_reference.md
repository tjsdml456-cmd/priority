# priority-3: QoS / Priority 코드 위치 참조

이 문서는 **DSCP 관측 → `dscp_qos_mapper` → 스케줄러에서 `runtime_qos` 덮어쓰기 → 우선순위 가중치** 흐름이 **어느 파일에 있는지**만 정리한다.  
(줄 번호는 문서 작성 시점의 `priority-3` 트리 기준이며, 편집 후에는 약간 어긋날 수 있다.)

---

## 흐름 요약

1. **초기 QoS**: RRC/UE 설정으로 내려온 논리 채널·5QI·ARP·GBR 정보가 `logical_channel_config`에 실리고, DU 스케줄러 UE의 DL/UL LC 매니저에 `configure()` 된다.
2. **DSCP 관측**: 주로 CU-UP SDAP DL TX에서 IPv4 ToS로 DSCP를 읽어 `dscp_qos_mapper`에 UE별로 저장한다.
3. **Override**: `scheduler_time_qos`가 매 슬롯 DL/UL 우선순위 계산 직전에 `apply_5qi_based_runtime_overrides()`를 호출해, 저장된 DSCP→5QI→표준 특성으로 `runtime_qos`(priority, PDB, res_type 등)를 갱신한다.
4. **스케줄 가중치**: `compute_dl_qos_weights` / `compute_ul_qos_weights`에서 `runtime_qos.priority`와 ARP 등을 섞어 최종 UE 후보 우선순위를 만든다.

---

## 1. `dscp_qos_mapper` (싱글톤, UE별 DSCP / DSCP→5QI)

| 내용 | 경로 | 대략적인 위치 |
|------|------|----------------|
| 클래스 정의, `register_dscp_for_ue`, `get_dscp_for_ue`, `map_dscp_to_5qi`, 내장 DSCP→5QI 테이블 | `include/srsran/sdap/dscp_qos_mapper.h` | 전체 (약 L37–L200) |

---

## 2. DSCP 등록 (트래픽에서 값이 들어오는 곳)

| 내용 | 경로 | 대략적인 위치 |
|------|------|----------------|
| DL: SDAP TX에서 IPv4 DSCP 추출 후 `register_dscp_for_ue`, 자동 5QI 매핑 보조 | `lib/sdap/sdap_entity_tx_impl.h` | `handle_sdu()` — 약 L84–L130 |
| Throughput 제어 경로에서 DSCP 등록 | `lib/sdap/throughput_controller.cpp` | `dscp_qos_mapper` 사용 — 약 L146, L156, L183–L184, L300 |
| DU 베어러 리소스 관리에서 mapper 참조 | `lib/du/du_high/du_manager/ran_resource_management/du_bearer_resource_manager.cpp` | 약 L214 근처 |

---

## 3. GTP-U (DSCP 파싱; mapper 등록과는 별도)

| 내용 | 경로 | 대략적인 위치 |
|------|------|----------------|
| UL TX: 내부 SDU에서 DSCP 추출 (주로 로그/추적용; 이 파일에서는 `register_dscp_for_ue` 호출 없음) | `lib/gtpu/gtpu_tunnel_ngu_tx_impl.h` | `gtpu_extract_dscp_from_ipv4_ul` 약 L41–L52, `handle_sdu` 약 L77–L101 |
| DL RX 쪽 IPv4 DSCP 추출 | `lib/gtpu/gtpu_tunnel_ngu_rx_impl.h` | `gtpu_extract_dscp_from_ipv4` 약 L39–, 사용 약 L256 근처 |

---

## 4. 스케줄러: 정책 진입점

| 내용 | 경로 | 대략적인 위치 |
|------|------|----------------|
| time-QoS 정책 API·`ue_ctxt` 내부 메서드 선언 | `lib/scheduler/policy/scheduler_time_qos.h` | `compute_ue_dl_priorities` / `compute_ue_ul_priorities` 약 L46–L50, `compute_dl_prio` / `compute_ul_prio` 약 L78–L79, `apply_5qi_based_runtime_overrides` 약 L97 |
| DL 후보별 우선순위 계산 루프 | `lib/scheduler/policy/scheduler_time_qos.cpp` | `compute_ue_dl_priorities` 약 L58–L70 |
| UL 후보별 우선순위 계산 루프 | 동일 | `compute_ue_ul_priorities` 약 L73–L85 |
| **DSCP 기반 `runtime_qos` 덮어쓰기** | 동일 | `apply_5qi_based_runtime_overrides` **약 L427–L562** |
| DL 우선순위: 평균 레이트 후 override 호출 | 동일 | `compute_dl_prio` 약 L575–L615 (`apply_5qi_based_runtime_overrides` 약 L584) |
| UL 우선순위: 동일 | 동일 | `compute_ul_prio` 약 L617–L678 (`apply_5qi_based_runtime_overrides` 약 L626) |
| DL 가중치: `runtime_qos.priority` × ARP 등 | 동일 | `compute_dl_qos_weights` 약 L156–L333 |
| UL 가중치 | 동일 | `compute_ul_qos_weights` 약 L335–L425 |
| 슬라이스 스케줄러에서 정책 호출 | `lib/scheduler/ue_scheduling/intra_slice_scheduler.cpp` | DL `compute_ue_dl_priorities` 약 **L403**, UL `compute_ue_ul_priorities` 약 **L431** |
| UL 그랜트 로그 등에서 `get_dscp_for_ue` | 동일 | 약 **L687** |

---

## 5. 초기 논리 채널 / QoS가 스케줄러 UE에 붙는 곳

| 내용 | 경로 | 대략적인 위치 |
|------|------|----------------|
| UE 설정 시 DL/UL LC 매니저 `configure(logical_channels())` | `lib/scheduler/ue_context/ue.cpp` | `set_config()` 내 약 **L119–L121** |
| DL LC에서 `runtime_qos` 기반 우선순위 읽기 등 | `lib/scheduler/ue_context/dl_logical_channel_manager.cpp` | `get_lc_prio` 약 L178–L184, `configure` 약 L191– |

---

## 6. 참고: upstream srsRAN과의 차이

- `srsRAN_CORE-1` 등 순정에 가까운 트리의 `lib/scheduler/policy/scheduler_time_qos.cpp`는 **줄 수가 훨씬 짧고**, `apply_5qi_based_runtime_overrides`·DSCP 연동이 **없을 수 있다**.
- 본 문서의 줄 번호는 **`priority-3`의 확장 버전**(약 725줄 규모의 `scheduler_time_qos.cpp`)을 기준으로 한다.

---

## 빠른 점프 체크리스트

- DSCP를 **어디에 저장**하나 → `include/srsran/sdap/dscp_qos_mapper.h` (`register_dscp_for_ue`)
- DL에서 **누가 등록**하나 → `lib/sdap/sdap_entity_tx_impl.h` (`handle_sdu`)
- 스케줄러가 **언제 덮어쓰나** → `lib/scheduler/policy/scheduler_time_qos.cpp` (`apply_5qi_based_runtime_overrides`, `compute_dl_prio` / `compute_ul_prio`에서 호출)
- **누가 정책을 호출**하나 → `lib/scheduler/ue_scheduling/intra_slice_scheduler.cpp` (DL/UL 각각)
