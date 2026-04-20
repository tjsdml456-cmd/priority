# iperf3 DSCP/rate 변경 관련 수정 요약

이 문서는 **iperf_QoS-1** 포크에서 DSCP 변경 + rate 변경 시나리오(0→44→24→15, 20/40/60초)가 정상 동작하도록 수정한 내용을 한 파일로 정리한 것이다.

---

## 1. 현상 (수정 전)

- **60초가 되면 트래픽이 멈춤** — `[DSCP_SEND]` 로그가 39초대에서 끊김.
- 40초에 rate 20M → 1M, 60초에 1M → 15M으로 바꿔도 **실제 송신은 40초 이후 거의 없음**.
- DSCP 15 구간(60~80초) 트래픽이 나가지 않음.

---

## 2. 원인

**Throttle( pacing ) 계산이 “테스트 시작부터의 누적”만 사용함.**

- 40초 시점: 이미 20Mbps × 40초 ≈ **800Mbit** 전송.
- 40초에 rate를 **1Mbps**로 변경.
- Throttle: `bits_sent / seconds` = 800M/40 = **20Mbps** → 목표 1M보다 훨씬 큼.
- “다음에 보낼 수 있는 시각” = `(800M − 40×1M) / 1M` = **760초 후**로 계산됨.
- 그 결과 40초 이후 사실상 **송신 정지** (green_light 안 줌).

즉, **고속 → 저속 rate 변경 시** 누적 전송량 때문에 “엄청 오래 sleep”하는 버그.

---

## 3. 수정 내용 (iperf_QoS-1)

### 3.1 Throttle 기준 리셋 (핵심 수정)

**파일:** `iperf.h`, `iperf_api.c`

- **`iperf_stream_result`에 throttle 기준 추가**
  - `throttle_baseline_time`: throttle 계산 기준 시각
  - `throttle_baseline_bytes`: 기준 시점의 누적 전송 바이트

- **테스트 시작 시** (`start_time_fixed` 설정하는 곳)
  - `throttle_baseline_time = now`, `throttle_baseline_bytes = bytes_sent` (0) 로 초기화.

- **Rate 변경 시** (`rate_change_timer_proc` 내부)
  - 새 rate 적용 직후:
    - `throttle_baseline_time = now`
    - `throttle_baseline_bytes = sp->result->bytes_sent`
  - 이후 pacing은 **“지금 시점부터”** 새 rate 기준으로만 계산.

- **`iperf_check_throttle`**
  - 기존: `start_time_fixed` ~ now, 전체 `bytes_sent` 사용.
  - 변경: `throttle_baseline_time` ~ now, `(bytes_sent - throttle_baseline_bytes)` 사용.
  - rate 변경 후에는 **baseline 이후 구간만** 보고 throttle 계산.

→ 40초에 1M, 60초에 15M으로 바꿔도 **그 시점부터** 해당 rate로만 pacing 되고, 트래픽이 끊기지 않음.

---

### 3.2 기타 적용된 수정 (참고)

| 항목 | 파일 | 내용 |
|------|------|------|
| **rate_flag** | `iperf_api.c` | `--rate-change` 사용 시 `rate_flag = 1` 설정해, init에서 rate가 UDP_RATE로 덮어씌워지지 않도록 함. |
| **rate=0 방어** | `iperf_api.c` | `rate_change_timer_proc`에서 `new_rate == 0`이면 rate 갱신 안 함 (green_light 멈춤 방지). |
| **DSCP 검증 로그** | `iperf_api.c`, `iperf_tcp.c`/`.h` | `get_socket_tos()` 추가. DSCP 변경 직후 `[DSCP_VERIFY]` 로그로 소켓 TOS 반영 여부 확인. |
| **송신 시 TOS 로그** | `iperf_api.c` | `[DSCP_SEND]` 로그로 실제 송신 시점의 `socket_tos`(DSCP) 확인 (약 1초 간격). |
| **RATE 파싱/타이머 로그** | `iperf_api.c` | `IPERF3_DSCP_DEBUG=1` 시 `[RATE_PARSE]`, `[RATE_FIRE]` 로그 추가. |
| **dscp_change_timer_proc 변수** | `iperf_api.c` | `[DSCP_VERIFY]` 블록에서 미선언 변수 `d` 사용 오류 수정 → `dv` 로컬 변수 사용. |

---

## 4. 사용 방법

- **디버그 로그:** `IPERF3_DSCP_DEBUG=1` 로 실행.
- **DL 전용 스크립트:** `priority-3/iperf3_dscp_traffic_80s.sh` (UL 제거, DL만 실행).
- **로그 확인 예:**
  ```bash
  grep -E '\[DSCP_|\[RATE_' /tmp/iperf3_dscp_scenario_dl0.log
  grep '\[DSCP_SEND\].*elapsed=6[0-9]\|elapsed=7[0-9]' /tmp/iperf3_dscp_scenario_dl0.log
  ```

---

## 5. 수정된 파일 목록 (iperf_QoS-1)

| 파일 | 변경 요약 |
|------|-----------|
| `src/iperf.h` | `iperf_stream_result`에 `throttle_baseline_time`, `throttle_baseline_bytes` 추가. |
| `src/iperf_api.c` | Throttle baseline 초기화/리셋, `iperf_check_throttle`에서 baseline 사용, rate_flag/rate=0 방어, DSCP/RATE 디버그 로그, `DSCP_VERIFY` 변수 수정. |
| `src/iperf_tcp.c` | `get_socket_tos()` 추가. |
| `src/iperf_tcp.h` | `get_socket_tos()` 선언 추가. |

**priority-3**

| 파일 | 변경 요약 |
|------|-----------|
| `iperf3_dscp_traffic_80s.sh` | UL 제거, DL 전용. `IPERF3_DSCP_DEBUG`를 sudo/netns에서도 전달하도록 `IPERF3_ENV` 사용. |
| `docs/iperf3_dscp_throughput_notes.md` | 60초 끊김/rate 갯수/clamp 관련 내용 및 디버그 방법 보강. |

---

**정리:** “60초에 트래픽이 안 나간다”는 현상은 **rate 변경 시 throttle 기준을 리셋하지 않아서** 40초 이후 pacing이 거의 무한대 sleep으로 잡힌 것이 원인이었고, **throttle baseline 도입 + rate 변경 시 리셋**으로 해결했다.
