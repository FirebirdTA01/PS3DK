#!/usr/bin/env python3
"""Compile ABI assertions with the actual PPU compiler; never install headers."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ps3dev', default=os.environ.get('PS3DEV') or os.environ.get('PS3DK'))
    parser.add_argument('--include', type=Path, default=root / 'sdk/include')
    parser.add_argument('--work-dir', type=Path)
    parser.add_argument('--require-toolchain', action='store_true')
    args = parser.parse_args()
    if not args.ps3dev:
        print('np-record-layout: SKIP (set PS3DEV or --ps3dev)')
        return int(args.require_toolchain)
    cc = Path(args.ps3dev) / 'ppu/bin/powerpc64-ps3-elf-gcc'
    if not cc.is_file():
        cc = cc.with_suffix('.exe')
    if not cc.is_file():
        parser.error('PPU compiler missing')
    target = subprocess.run([str(cc), '-dumpmachine'], text=True,
                            capture_output=True, timeout=30, check=True)
    if target.stdout.strip() != 'powerpc64-ps3-elf':
        parser.error('not a PS3 PPU compiler')
    work = args.work_dir
    if work is None:
        with tempfile.TemporaryDirectory(prefix='np-layout-') as temp:
            return matrix(cc, args.include, Path(temp), root)
    work.mkdir(parents=True, exist_ok=False)
    return matrix(cc, args.include, work, root)


def matrix(cc, inc, work, root):
    fails = 0
    src = root / 'tests/sdk/np-record-layout-test.c'
    for lang, std in [('c', 'gnu99'), ('c', 'c11'), ('c++', 'c++17')]:
        for abi in ['ilp32', 'lp64']:
            label = f'{std}-{abi}'
            cmd = [str(cc), '-x', lang, '-std=' + std, '-O2', '-Wall',
                   '-Wextra', '-Werror',
                   '-DEXPECTED_ABI_BYTES=' + ('8' if abi == 'lp64' else '4'),
                   '-I' + str(inc)]
            if abi == 'lp64':
                cmd.append('-mlp64')
            cmd += [str(src), '-o', str(work / (label + '.elf'))]
            result = subprocess.run(cmd, capture_output=True, text=True,
                                    errors='replace', timeout=60)
            (work / (label + '.log')).write_text(
                repr(cmd) + '\n' + result.stdout + result.stderr,
                encoding='utf-8')
            print(f'np-record-layout: {"FAIL" if result.returncode else "PASS"} {label}')
            fails += bool(result.returncode)
    print(f'np-record-layout: {6 - fails}/6 PASS')
    if not fails:
        fails += mutants(cc, inc, work, src)
    return int(fails != 0)


def mutants(cc, inc, work, src):
    """Each old layout defect and witness corruption must fail its own diagnostic."""
    nl = chr(10)
    cases = [
        ('userinfo2-lp64-pointer', 'cell/np/common.h',
         '    SceNpOnlineName *onlineName ATTRIBUTE_PRXPTR;' + nl + '    SceNpAvatarUrl *avatarUrl ATTRIBUTE_PRXPTR;',
         '    SceNpOnlineName *onlineName;' + nl + '    SceNpAvatarUrl *avatarUrl;',
         'SceNpUserInfo2 size', ['lp64']),
        ('clans-basicinfo-nummembers', 'cell/np/clans.h',
         '    SceNpClanId clanId;' + nl + '    uint32_t numMembers;',
         '    SceNpClanId clanId;' + nl + '    size_t numMembers;',
         'SceNpClansClanBasicInfo.numMembers width', ['lp64']),
        ('clans-claninfo-bindata', 'cell/np/clans.h',
         '    void *binData1 ATTRIBUTE_PRXPTR;' + nl + '    uint32_t binData1Size;' + nl + '} SceNpClansUpdatableClanInfo;',
         '    uint8_t binData1[SCE_NP_CLANS_CLAN_BINARY_DATA_MAX_SIZE];' + nl + '    uint32_t binData1Size;' + nl + '} SceNpClansUpdatableClanInfo;',
         'SceNpClansUpdatableClanInfo size', ['ilp32', 'lp64']),
        ('clans-msgdata-bindata', 'cell/np/clans.h',
         '    void *binData1 ATTRIBUTE_PRXPTR;' + nl + '    uint32_t binData1Size;' + nl + '} SceNpClansMessageData;',
         '    uint8_t binData1[SCE_NP_CLANS_MESSAGE_BINARY_DATA_MAX_SIZE];' + nl + '    uint32_t binData1Size;' + nl + '} SceNpClansMessageData;',
         'SceNpClansMessageData size', ['ilp32', 'lp64']),
        ('commerce2-session-padding', 'cell/np/commerce2.h',
         '    uint8_t symbolWithSpace;' + nl + '    char thousandSeparator[',
         '    uint8_t symbolWithSpace;' + nl + '    uint8_t pad[3];' + nl + '    char thousandSeparator[',
         'SceNpCommerce2SessionInfo.thousandSeparator offset', ['ilp32', 'lp64']),
        ('commerce2-missing-ov', 'cell/np/commerce2.h',
         '    void *data2 ATTRIBUTE_PRXPTR;' + nl + '    uint32_t ov;' + nl + '    uint8_t reserved[12];',
         '    void *data2 ATTRIBUTE_PRXPTR;' + nl + '    uint8_t reserved[12];',
         'SceNpCommerce2CommonData.reserved offset', ['ilp32', 'lp64']),
        ('matching2-embedded-session', 'cell/np2/matching2.h',
         '    SceNpMatching2JoinedSessionInfo *joinedSessionInfo ATTRIBUTE_PRXPTR;',
         '    SceNpMatching2JoinedSessionInfo joinedSessionInfo;',
         'SceNpMatching2UserInfo size', ['ilp32', 'lp64']),
        ('witness-always-fail', 'test:',
         '    return 0;' + nl + '}',
         '    np_witness_decode_mismatch();' + nl + '    return 0;' + nl + '}',
         'np_witness_decode_mismatch', ['ilp32', 'lp64']),
        ('witness-corrupted-address', 'test:',
         'user_buf[39] = 0x78;',
         'user_buf[39] = 0x99;',
         'np_witness_decode_mismatch', ['ilp32', 'lp64']),
        ('witness-corrupted-count', 'test:',
         'clan_buf[475] = 0x00;',
         'clan_buf[475] = 0x99;',
         'np_witness_decode_mismatch', ['ilp32', 'lp64']),
    ]
    fails = 0
    for label, target_file, old, new, diagnostic, abis in cases:
        if target_file == 'test:':
            text = src.read_text(encoding='utf-8')
            if text.count(old) != 1:
                print(f'np-record-layout: FAIL cannot plant {label}')
                fails += 1
                continue
            mutant_src = work / f'{label}.c'
            mutant_src.write_text(text.replace(old, new), encoding='utf-8')
            cur_src = mutant_src
            overlay_args = []
        else:
            text = (inc / target_file).read_text(encoding='utf-8')
            if text.count(old) != 1:
                print(f'np-record-layout: FAIL cannot plant {label}')
                fails += 1
                continue
            overlay = work / label
            out = overlay / target_file
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_text(text.replace(old, new), encoding='utf-8')
            cur_src = src
            overlay_args = ['-I' + str(overlay)]
        for abi in abis:
            cmd = [str(cc), '-x', 'c', '-std=c11', '-O2', '-Wall', '-Wextra',
                   '-Werror', '-DEXPECTED_ABI_BYTES=' + ('8' if abi == 'lp64' else '4')]
            cmd += overlay_args
            cmd += ['-I' + str(inc)]
            if abi == 'lp64':
                cmd.append('-mlp64')
            cmd += [str(cur_src), '-o', str(work / 'mutant.elf')]
            result = subprocess.run(cmd, capture_output=True, text=True,
                                    errors='replace', timeout=60)
            (work / f'{label}-{abi}.log').write_text(
                result.stdout + result.stderr, encoding='utf-8')
            ok = result.returncode != 0 and diagnostic in result.stderr
            print(f'np-record-layout: {"PASS" if ok else "FAIL"} mutant {label}-{abi}')
            fails += not ok
    return fails


if __name__ == '__main__':
    raise SystemExit(main())
