# iperf3 DSCP/rate-change 및 4 Mbps throughput 관련 정리

## 1. iperf3 시간/API (--dscp-change, --rate-change)

### 1.1 표준 iperf3
- 공식 ESnet iperf3 문서에는 **`--dscp-change`**, **`--rate-change`** 옵션이 없음.
- 사용 중인 바이너리가 **커스텀/포크**일 가능성이 있음. 해당 소스에서 아래를 반드시 확인할 것.

### 1.2 확인할 것 (소스 기준)

**DSCP=15가 안 찍히는 문제 (마지막 구간):**

- **인자 해석**
  - `DSCP_CHANGE_ARGS="0,20,44,40,24,60,15"` → (시작DSCP, t1, d1, t2, d2, t3, d3) 형태일 때:
    - `t>=60` 구간에서 DSCP=15가 적용되는지, 아니면 **“60초 시점에 한 번만 15로 바꾸고 이후는 적용 안 함”** 같은 해석인지.
  - **마지막 (time, value) 쌍**이 “그 시각까지 유효”로만 쓰이고, **종료 시각(80초) 전까지 유지”** 로 처리되는지 코드로 확인.

- **타이머/시간 비교**
  - 테스트 종료 시각 `TOTAL_DUR=80` 과 비교할 때:
    - `elapsed >= 60` 이면 DSCP=15 적용,
    - 또는 “다음 변경 시각이 80 초과면 마지막 변경을 적용하지 않음” 같은 로직이 있는지.
  - UDP send 루프에서 **매 패킷/블록마다** `elapsed` 기준으로 현재 DSCP/rate를 갱신하는지, 아니면 **구간 전환 시점에만** 갱신하는지.

**권장:** 사용 중인 iperf3 소스에서 `dscp_change` / `rate_change` / `timer` / `elapsed` 검색 후, “마지막 (60, 15) 적용 구간”과 “80초까지 15 유지” 여부를 확인.

### 1.3 UDP에서 DSCP/마지막 구간 트래픽 끊김 (iperf_QoS-1)

- **증상**: DSCP가 15(마지막 구간)로 바뀌자마자 트래픽이 안 감.
- **원인**: DSCP 변경 시 **버퍼 플러시**(SO_SNDBUF 1024로 축소 + **usleep(0.5초)**)가 송신 스레드에서 실행되어, 0.5초 동안 송신이 멈추고 버퍼가 작아져 UDP 처리량이 급감할 수 있음.
- **수정 (1)**: **UDP**인 경우 버퍼 플러시를 하지 않도록 함 (`test->protocol->id == Pudp` 이면 `skip_buffer_flush = 1`). TCP만 플러시 사용. 환경변수 `IPERF3_DSCP_NO_BUFFER_FLUSH=1` 로도 플러시 비활성화 가능.
- **수정 (2)**: `settings->rate == 0` 이면 `iperf_check_throttle` 이 green_light를 주지 않아 송신이 멈춤. rate 타이머에서 `new_rate == 0` 이면 rate를 갱신하지 않음. 그리고 throttle에서 rate==0 이면 green_light=1 로 두어 “제한 없음”으로 동작하도록 함.

### 1.4 마지막 DSCP가 안 들어갈 때 원인 찾기 (디버그 로그)

- **방법**: iperf_QoS-1 빌드 후, **`IPERF3_DSCP_DEBUG=1`** 을 주고 클라이언트 실행. stderr에 다음이 찍힘:
  - **`[DSCP_PARSE]`**: 파싱 직후 `count`, `dscp_count`, `values[]`, `times[]`.  
    → `dscp_count=4`, `values=[0,44,24,15]`, `times=[20,40,60]` 이면 파싱은 정상.
  - **`[DSCP_TIMER]`**: 타이머 생성 시마다 `i`, `time`, `delay_usecs`, 적용할 `dscp_values[i+1]`.  
    → `i=2` 에 대해 한 줄이 있고 `SKIP (past)` 가 아니면 60초 타이머가 생성된 것.
  - **`[DSCP_FIRE]`**: 콜백 호출 시마다 `dscp_index`, `dscp_count`, `APPLY`/`SKIP`, `new_tos`.  
    → 60초 근처에 `dscp_index=3` / `APPLY` / `new_tos=60`(DSCP 15의 TOS) 이 나와야 마지막 DSCP 적용됨.
- **해석**:  
  - PARSE에서 마지막 값(15)이 안 보이면 → 파싱/개수 버그.  
  - TIMER에서 `i=2` 가 없거나 SKIP이면 → 타이머가 안 만들어지거나 과거로 스킵.  
  - FIRE에서 `dscp_index=3` 이 안 나오면 → 3번째 타이머가 안 돌고 있음.
- 예: `IPERF3_DSCP_DEBUG=1 iperf3 -c ... --dscp-change "0,20,44,40,24,60,15" 2>&1 | tee dscp.log`

### 1.5 갯수 문제 / 60초에 트래픽 끊김 (iperf_QoS-1)

- **증상**: DSCP/rate 구간을 늘리면(예: 4구간) "그 후로" DSCP가 안 들어감. 또는 **60초가 되면 트래픽이 안 돌아감** (초가 아니라 **rate-change 횟수** 문제일 수 있음).
- **원인 후보**:
  1. **rate_flag 미설정**: `--rate-change`만 쓰고 `-b`를 안 쓰면 `rate_flag`가 0이라, init에서 `settings->rate`가 `UDP_RATE`로 덮어씌워짐. → **수정**: OPT_RATE_CHANGE 파싱 시 `rate_flag = 1` 설정.
  2. **4번째 구간에서 rate=0**: rate 타이머가 `rate_values[3]`를 0으로 적용하면 `iperf_check_throttle`이 green_light를 주지 않아 송신이 멈춤. → **수정**: `new_rate == 0`이면 rate를 갱신하지 않음 + `[RATE_FIRE] SKIP` 로그.
  3. gNB 쪽에서 DSCP/rate **갯수**에 대한 clamp나 상한이 있으면, 4번째 구간에서만 드랍/무시될 수 있음 (priority-3 gNB 코드에서 count 제한 검색 필요).
- **디버그 (rate 쪽)**  
  `IPERF3_DSCP_DEBUG=1` 시 stderr에 추가로:
  - **`[RATE_PARSE]`**: `rate_count`, `values[]`, `times[]` → 4구간이면 `rate_count=4`, `times=[20,40,60]`, 마지막 value 15M 확인.
  - **`[RATE_FIRE]`**: 각 rate 타이머 fire 시 `rate_index`, `new_rate` (또는 `SKIP (new_rate=0)`).  
  → 60초 근처에 `rate_index=3` / `15000000 bps` 가 나오면 iperf3는 4번째 rate 적용한 것. 안 나오면 타이머 미생성/과거 스킵/파싱 오류.
- **대응**: (1) `IPERF_MAX_DSCP_RATE_TIMERS` 4 → 6. (2) DSCP 타이머는 `dscp_count`만, rate는 `rate_count`만 사용해 생성·상한 cap. 리눅스에서 해당 소스로 다시 빌드·설치 필요.

---

## 2. GBR 20 Mbps인데 4 Mbps만 나오는 현상

### 2.1 우리 코드에서 GBR의 역할
- **scheduler_time_qos**: `gbr_dl = 20e6` (20 Mbps)는 **스케줄링 가중치(gbr_weight)** 계산용.
- `gbr_weight = gbr_dl / dl_avg_rate` → 평균 전송률이 20 Mbps보다 낮으면 가중치가 커져서 **우선순위만 올라감**.
- **실제로 스케줄러가 내보내는 양**은 **RLC에 쌓인 데이터(pending_dl_newtx_bytes)** 와 셀 용량·다른 UE에 의해 결정됨.
- 즉, **GBR는 “목표 비트레이트”이지, DU에 데이터를 만들어 주거나 보장하는 값이 아님.**

### 2.2 4 Mbps 한계가 날 수 있는 지점

| 구간 | 설명 |
|------|------|
| **iperf3 클라이언트** | `--rate-change` 해석 오류, UDP send 버퍼 작음, 또는 초기/구간별 rate가 실제로 20M가 아님. |
| **경로 (클라이언트 → Core)** | 중간 구간에서 rate limit, 큐 드랍 등으로 4 Mbps 수준으로만 전달. |
| **Core/UPF → DU (N3/F1-U)** | N3/F1-U 구간 또는 UPF 설정으로 인해 DU로 들어오는 DL 유량이 4 Mbps 수준으로 제한. |
| **DU 스케줄러** | UE0가 GBR로 우선순위는 높지만, **DU에 들어온 데이터 자체가 4 Mbps 분량**이면 그 이상은 스케줄할 수 없음. |

### 2.3 iperf3 버퍼/전송률 쪽 확인 (코드에서 볼 것)

- **UDP send 루프**
  - 목표 rate(예: 20M)를 어떻게 맞추는지 (슬립, 블록 크기, 반복 횟수).
  - `--rate-change` 시 **구간별로 목표 rate가 실제로 20M으로 설정되는지** (초기값/마지막 구간 오류 가능성).
- **버퍼**
  - send 버퍼 크기: 작으면 버스트가 잘리거나 실제 throughput이 rate보다 낮게 나올 수 있음.
  - `-l` (length) / 블록 크기: 너무 크면 지연/throughput 변동에 영향을 줄 수 있음.
- **실측**
  - iperf3 **서버** 출력의 “받은 비트레이트”와, **클라이언트** 출력의 “보낸 비트레이트”를 비교.
  - 가능하면 **클라이언트/서버 앞단 tcpdump** 로 초당 패킷 수 × 패킷 크기 = 실제 전송률 확인.

### 2.4 DU 쪽에서 추가로 볼 것
- **DL buffer status**: UE0 LCID4에 대한 BSR이 20 Mbps를 감당할 만큼 큰지 (데이터가 계속 들어오는지).
- **스케줄링**: UE0가 GBR로 우선순위가 높은 구간에서도, 다른 UE와 셀 용량 때문에 한 슬롯에 나가는 비트 수가 제한될 수 있음. 단, “4 Mbps로 고정”이라면 **DU에 들어오는 데이터량이 4 Mbps 근처**일 가능성이 더 큼.

---

## 3. 요약

- **마지막 DSCP=15가 안 찍히는 것**: 사용 중인 iperf3의 **시간/구간 해석**(마지막 (60,15) 적용 여부, 80초까지 유지 여부)을 소스에서 확인하는 것이 좋음.
- **GBR 20 Mbps인데 4 Mbps만 나오는 것**: 우리 스케줄러는 GBR를 “우선순위용 목표”로만 쓰므로, **실제 한계는 (1) iperf3가 20M으로 보내는지, (2) Core/UPF→DU 구간이 20M을 넘겨주는지**를 먼저 확인해야 함. iperf3 버퍼/전송률 로직과 경로 측정(tcpdump, iperf 수신률)을 함께 보는 것을 권장.
