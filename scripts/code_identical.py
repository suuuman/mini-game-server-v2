# -*- coding: utf-8 -*-
r"""
주석만 고쳤는지 확인한다 — 주석과 공백을 걷어낸 「코드 본문」이 기준과 같은지 본다.

    python scripts\code_identical.py <기준-리비전>
    예) python scripts\code_identical.py HEAD

문자열 리터럴 안의 // 나 /* 는 주석이 아니므로 상태 기계로 훑는다.
"""
import hashlib
import subprocess
import sys

# 콘솔 기본 코드페이지가 cp949 라 한글·대시가 그냥은 안 찍힌다.
try:
    sys.stdout.reconfigure(encoding='utf-8')
except AttributeError:
    pass


def strip_comments(src: str) -> str:
    out = []
    i, n = 0, len(src)
    state = 'code'          # code | line | block | str | chr
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ''

        if state == 'code':
            if c == '/' and nxt == '/':
                state = 'line'; i += 2; continue
            if c == '/' and nxt == '*':
                state = 'block'; i += 2; continue
            if c == '"':
                state = 'str'; out.append(c); i += 1; continue
            if c == "'":
                # C++14 숫자 구분자(100'000)의 ' 는 문자 리터럴이 아니다.
                # 앞뒤가 영숫자면 구분자로 본다 — 아니면 여기서 상태가 갇혀
                # 그 뒤의 주석이 통째로 코드로 취급된다.
                prev = src[i - 1] if i else ''
                if prev.isalnum() and nxt.isalnum():
                    out.append(c); i += 1; continue
                state = 'chr'; out.append(c); i += 1; continue
            out.append(c); i += 1; continue

        if state == 'line':
            if c == '\n':
                state = 'code'; out.append(c)
            i += 1; continue

        if state == 'block':
            if c == '*' and nxt == '/':
                state = 'code'; i += 2; continue
            if c == '\n':
                out.append(c)      # 줄 수는 어차피 정규화로 사라진다
            i += 1; continue

        # 리터럴 안 — 이스케이프를 건너뛴다
        out.append(c)
        if c == '\\' and nxt:
            out.append(nxt); i += 2; continue
        if (state == 'str' and c == '"') or (state == 'chr' and c == "'"):
            # 여는 따옴표 자신은 위에서 이미 넣었으므로, 닫는 것만 여기로 온다
            if len(out) >= 2:
                state = 'code'
        i += 1

    # 공백 정규화 — 들여쓰기·줄바꿈 차이는 무시한다
    return ' '.join(''.join(out).split())


def digest(src: str) -> str:
    return hashlib.sha256(strip_comments(src).encode('utf-8')).hexdigest()[:16]


def main() -> int:
    base = sys.argv[1] if len(sys.argv) > 1 else 'HEAD'

    files = subprocess.run(
        ['git', 'ls-files', 'src'], capture_output=True, text=True,
        encoding='utf-8', check=True).stdout.split()
    files = [f for f in files if f.endswith(('.cpp', '.h'))]

    bad = []
    for f in files:
        try:
            old = subprocess.run(['git', 'show', f'{base}:{f}'], capture_output=True,
                                 text=True, encoding='utf-8', check=True).stdout
        except subprocess.CalledProcessError:
            print(f'  ?  {f} — {base} 에 없다 (신규)')
            continue
        with open(f, encoding='utf-8') as fp:
            new = fp.read()

        a, b = digest(old), digest(new)
        if a != b:
            bad.append(f)
            print(f'  !! {f}  {a} -> {b}')

    print('-' * 60)
    if bad:
        print(f'코드가 바뀐 파일 {len(bad)} 개 — 주석만 고친 것이 아니다')
        return 1
    print(f'검사 {len(files)} 파일 — 코드 본문 동일. 주석만 바뀌었다')
    return 0


if __name__ == '__main__':
    sys.exit(main())
