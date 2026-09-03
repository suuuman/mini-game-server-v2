# scripts\send.ps1 — 원시 바이트를 보내고, 「몇 번에 나눠서」 오갔는지를 본다
#
#   -Framed 없이  : 헤더 없는 raw 바이트. 뭉침/쪼개짐이 그대로 드러난다
#   -Framed 붙이면: packet.h 와 같은 규약으로 헤더를 붙인다
#
#   뭉침   : .\send.ps1 -Repeat 20 -Size 4
#   대조군 : .\send.ps1 -Repeat 20 -Size 4 -NoDelay
#   쪼개짐 : .\send.ps1 -Repeat 1  -Size 50000
#   복원   : .\send.ps1 -Repeat 20 -Size 4 -Framed
#
#   주의: Repeat*Size 를 60000 이상으로 올리지 말 것.
#         클라이언트가 다 쓸 때까지 안 읽는 구조라 양쪽 버퍼가 차면 에코 데드락.

param(
    [int]$Port    = 9000,
    [int]$Repeat  = 3,
    [int]$Size    = 4,
    [int]$MsgId   = 1,          # proto::MsgId::kEchoReq
    [int]$Timeout = 1000,
    [int]$Hold    = 0,          # 주고받은 뒤 연결을 이 초만큼 열어 둔다 (동시 접속 확인용)
    [int]$Split   = 0,          # 한 메시지를 이 개수로 쪼개 보낸다 (경계 복원 확인용)
    [int]$SplitMs = 60,         # 쪼갠 조각 사이 간격 ms
    [int]$RawSize   = -1,       # 헤더의 body_size 에 이 값을 강제로 박는다 (정수 넘침 실습)
    [int]$ReadDelay = 0,        # 다 보낸 뒤 이 ms 만큼 안 읽는다 (서버 송신을 밀리게 해서 배치 관찰)
    [switch]$Seq,               # 본문에 순번을 박고 돌아온 순서를 검사한다 (Framed · Size>=4 필요)
    [switch]$NoDelay,           # 붙이면 Nagle 을 끈다 (대조군)
    [switch]$Framed,            # 붙이면 [body_size:u16 BE][msg_id:u16 BE] 를 앞에 단다
    [switch]$DropAfterSend      # 보내자마자 RST 로 끊는다 — 응답을 안 받는다
)

$ErrorActionPreference = 'Stop'

function New-Frame([byte[]]$Body, [int]$Id) {
    $h = [byte[]]::new(4)
    $h[0] = [byte](($Body.Length -shr 8) -band 0xFF)   # 상위 바이트가 먼저 = 빅엔디언
    $h[1] = [byte]( $Body.Length         -band 0xFF)
    $h[2] = [byte](($Id          -shr 8) -band 0xFF)
    $h[3] = [byte]( $Id                  -band 0xFF)
    return $h + $Body
}

$client = [System.Net.Sockets.TcpClient]::new("127.0.0.1", $Port)
$client.NoDelay = [bool]$NoDelay
$stream = $client.GetStream()
$stream.ReadTimeout = $Timeout

$body = [System.Text.Encoding]::ASCII.GetBytes('A' * $Size)
$unit = if ($Framed) { New-Frame $body $MsgId } else { $body }

# -RawSize : 헤더의 body_size 에 「실제 본문 길이와 다른 값」을 강제로 박는다.
#   상한 검사가 제대로 되어 있는지, 정수 넘침으로 뚫리는지 보는 데 쓴다.
#   예) -RawSize 65533  →  4 + 65533 이 uint16 에서 1 로 접힌다
#       -RawSize 60000  →  상한(4096) 초과라 정상 서버는 끊어야 한다
if ($Framed -and $RawSize -ge 0) {
    $unit[0] = [byte](($RawSize -shr 8) -band 0xFF)
    $unit[1] = [byte]( $RawSize         -band 0xFF)
    "raw   : body_size 를 $RawSize 로 위조 (실제 본문 $Size B)"
}
$expected = $Repeat * $unit.Length

"send  : $Repeat x $($unit.Length)B = $expected B   framed=$($Framed.IsPresent)  nodelay=$($client.NoDelay)"

if ($Seq) { "seq   : 본문 앞 4바이트에 순번을 박고, 돌아온 순서를 검사한다" }

for ($i = 0; $i -lt $Repeat; $i++) {

    # -Seq : 본문 앞 4바이트에 순번(빅엔디언)을 박는다.
    #   에코 서버라 그대로 돌아오므로, 받은 순번이 0,1,2,... 인지로
    #   송신 순서가 보장되는지 검사할 수 있다.
    if ($Seq -and $Framed -and $Size -ge 4) {
        $unit[4] = [byte](($i -shr 24) -band 0xFF)
        $unit[5] = [byte](($i -shr 16) -band 0xFF)
        $unit[6] = [byte](($i -shr  8) -band 0xFF)
        $unit[7] = [byte]( $i          -band 0xFF)
    }

    if ($Split -gt 1) {
        # 한 메시지를 여러 조각으로 나눠, 사이를 벌려서 보낸다.
        #   서버 쪽에서는 completion 이 여러 번 나뉘어 오고, 그래도 프레임이
        #   하나로 복원되어야 한다. (L4 DoD 2)
        $chunk = [math]::Ceiling($unit.Length / $Split)
        $off = 0
        while ($off -lt $unit.Length) {
            $n = [math]::Min($chunk, $unit.Length - $off)
            $stream.Write($unit, $off, $n)
            $stream.Flush()
            $off += $n
            if ($off -lt $unit.Length) { Start-Sleep -Milliseconds $SplitMs }
        }
    } else {
        $stream.Write($unit, 0, $unit.Length)
    }
}
$stream.Flush()

# 일부러 안 읽고 버틴다.
#   내 수신 버퍼와 서버의 송신 버퍼가 차면 서버의 WSASend 가 pending 으로 남고,
#   그동안 들어온 응답들이 서버 송신 큐에 쌓인다. 그게 배치로 나가는 걸 보려는 것.
if ($ReadDelay -gt 0) {
    "delay : $ReadDelay ms 동안 읽지 않는다 (서버 송신을 일부러 밀리게 한다)"
    Start-Sleep -Milliseconds $ReadDelay
}

# ── 보내자마자 끊는다 ────────────────────────────────────
#  응답을 기다리지 않고 즉시 끊어서, 서버 쪽에
#  「로직 스레드는 아직 이 세션을 처리 중인데 I/O 워커는 이미 소켓을 닫았다」
#  는 창을 만든다. 그 창이 이 옵션으로 보려는 것이다.
#
#  FIN 이 아니라 RST 인 이유 —
#    LingerState(enabled=$true, 0초) 이면 Close 가 FIN 대신 RST 를 보낸다.
#    서버에 걸려 있던 WSARecv 가 「정상 완료(0바이트)」가 아니라 「실패 완료」로
#    즉시 돌아와서, 창이 더 빨리·더 확실히 열린다.
if ($DropAfterSend) {
    $client.LingerState = [System.Net.Sockets.LingerOption]::new($true, 0)
    $client.Close()
    "drop  : RST 로 즉시 종료 — 응답을 받지 않는다"
    return
}

# ── 핵심은 총량이 아니라 「읽은 횟수」다 ──────────────────────────────
$buf   = [byte[]]::new(65536)
$sink  = [System.IO.MemoryStream]::new()
$reads = 0
try {
    while ($sink.Length -lt $expected) {
        $n = $stream.Read($buf, 0, $buf.Length)
        if ($n -le 0) { break }
        $reads++
        "  read #$reads : $n bytes"
        $sink.Write($buf, 0, $n)
    }
} catch [System.IO.IOException] {
    "  (read timeout — 더 올 게 없음)"
}

$recvd = $sink.ToArray()
"total : $($recvd.Length) B in $reads reads   (expected $expected B)"

# ── 헤더를 붙여 보냈다면, 받은 쪽도 경계를 복원해서 「패킷 몇 개」인지 센다 ──
if ($Framed) {
    $off = 0
    $packets = 0
    $seqs = New-Object System.Collections.Generic.List[int]
    while ($off + 4 -le $recvd.Length) {
        # [int] 캐스팅이 필수다.
        #    PowerShell 의 -shl 은 왼쪽 피연산자가 [byte] 면 결과도 [byte] 로 잘라낸다.
        #    [byte]15 -shl 8 == 0 이다 (3840 이 아니다).
        #    이걸 빠뜨리면 body_size 의 상위 바이트가 통째로 사라져서,
        #    「서버가 스트림을 깨뜨렸다」는 완전한 거짓 진단이 나온다. 실제로 겪었다.
        $len = ([int]$recvd[$off]     -shl 8) -bor [int]$recvd[$off + 1]
        $id  = ([int]$recvd[$off + 2] -shl 8) -bor [int]$recvd[$off + 3]
        if ($off + 4 + $len -gt $recvd.Length) {
            "  ! 불완전 프레임 — $($recvd.Length - $off) B 남음 (다음 recv 를 기다려야 하는 상황)"
            break
        }
        $packets++
        if ($packets -le 3) { "  frame #$packets : id=$id body=$len" }
        if ($Seq -and $len -ge 4) {
            $seqs.Add( (([int]$recvd[$off+4] -shl 24) -bor ([int]$recvd[$off+5] -shl 16) -bor
                        ([int]$recvd[$off+6] -shl  8) -bor  [int]$recvd[$off+7]) )
        }
        $off += 4 + $len
    }
    "frames: $packets packets   (expected $Repeat)"

    # 순서 검사 — 서버가 같은 소켓에 WSASend 를 여러 개 동시에 걸면
    #   여기서 중복·누락·뒤바뀜으로 드러난다.
    if ($Seq -and $seqs.Count -gt 0) {
        $out = 0
        for ($k = 0; $k -lt $seqs.Count; $k++) { if ($seqs[$k] -ne $k) { $out++ } }
        $dup = $seqs.Count - ($seqs | Select-Object -Unique).Count

        if ($out -eq 0 -and $dup -eq 0 -and $seqs.Count -eq $Repeat) {
            "order : OK — $($seqs.Count) 개가 0..$($Repeat-1) 순서대로"
        } else {
            "order : X  어긋남 $out 개 · 중복 $dup 개 · 받은 개수 $($seqs.Count)/$Repeat"
            "        받은 순번(앞 24): " + (($seqs | Select-Object -First 24) -join ',')
        }
    }
}

# -Hold 를 주면 연결을 붙잡고 있는다.
# 창을 여러 개 띄워 동시 접속을 만들거나, 한 창만 강제 종료해서
# 다른 세션이 멀쩡한지 보는 데 쓴다. (L2 DoD 1·2)
if ($Hold -gt 0) {
    "hold  : $Hold 초 동안 연결 유지 — 이 창을 강제로 닫으면 그 세션만 끊긴다"
    Start-Sleep -Seconds $Hold
}

$client.Close()
