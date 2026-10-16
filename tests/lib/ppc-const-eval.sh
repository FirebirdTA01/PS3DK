# Straight-line PPU integer evaluator for assembly checks, sourced by tests (bash 4+, 64-bit arithmetic).
#
# ppc_eval <body> [<stop regex>]
#   Runs the instructions of one function body in order (the caller cuts the body: label to blr) and stops at the
#   first line matching <stop regex> (default: none, i.e. the end).  Tracks general registers as known 64-bit values
#   or unknown, and 8-byte stack slots addressed off(base) while the base register is unchanged.
#   Known: li, lis, addi (rA 0 = literal), mr, ori, oris, sldi, rldimi, rldicl/clrldi, std / ld of a tracked slot.
#   Every other instruction that writes a general register (its first operand) makes it unknown; compares, vector
#   loads (vector destination) and directives write no general register; an update form (stdu/stwu) makes its base
#   unknown.  Reading an unknown register makes the result unknown.
#   Fail closed: any branch, call or conditional return before the stop line makes EVERY result unknown
#   (PPC_UNSUPPORTED holds it); any std forgets every other tracked slot first and any other store forgets them all,
#   so an aliasing base or an overlapping narrower store can never leave a stale value behind.
#   Afterwards ppc_reg <n> prints the value in hex (0x%016x) or "unknown", and PPC_STOP_LINE holds the stop line.
#
# ppc_ea <instruction line>: for an X-form access "op VR,RA,RB" after ppc_eval stopped on it, prints the effective
#   address ((RA = 0 ? 0 : rA) + rB) in hex, or "unknown".

declare -A PPC_R PPC_M
PPC_STOP_LINE=""
PPC_UNSUPPORTED=""

ppc_imm() { # <text>: a decimal or 0x hex immediate, or empty if not one
    local v=$1
    [[ $v =~ ^-?(0x[0-9a-fA-F]+|[0-9]+)$ ]] || return 1
    echo $(( v ))
}
ppc_get() { [[ -n "${PPC_R[$1]+x}" ]] && echo "${PPC_R[$1]}"; }
ppc_set() { PPC_R[$1]=$2; ppc_drop_slots "$1"; }
ppc_kill() { unset 'PPC_R[$1]'; ppc_drop_slots "$1"; }
ppc_drop_slots() { local k; for k in "${!PPC_M[@]}"; do [[ $k == *"($1)" ]] && unset 'PPC_M[$k]'; done; return 0; }
ppc_mask() { # <mb> <me> (IBM bit numbers, 0 = MSB, mb <= me)
    local w=$(( $2 - $1 + 1 ))
    if (( w >= 64 )); then echo -1; else echo $(( ((1 << w) - 1) << (63 - $2) )); fi
}
ppc_rotl() { # <value> <n>
    local x=$1 n=$(( $2 & 63 ))
    if (( n == 0 )); then echo "$x"; else echo $(( (x << n) | ((x >> (64 - n)) & ((1 << n) - 1)) )); fi
}

ppc_eval() {
    local body=$1 stop=${2:-} line op a b c d va vb v m
    PPC_R=(); PPC_M=(); PPC_STOP_LINE=""; PPC_UNSUPPORTED=""
    while IFS= read -r line; do
        line=${line%%#*}; line=${line//$'\r'/}
        line=$(echo "$line" | sed -E 's/^[[:space:]]+//; s/[[:space:]]+$//')
        [[ -z $line || $line == .* || $line == *: ]] && continue
        if [[ -n $stop && $line =~ $stop ]]; then PPC_STOP_LINE=$line; return 0; fi
        op=${line%%[[:space:]]*}
        IFS=, read -r a b c d <<< "$(echo "${line#"$op"}" | tr -d '[:space:]')"
        case $op in
            li)    if v=$(ppc_imm "$b"); then ppc_set "$a" $(( (v << 48) >> 48 )); else ppc_kill "$a"; fi ;;
            lis)   if v=$(ppc_imm "$b"); then ppc_set "$a" $(( ((v & 0xffff) << 48) >> 32 )); else ppc_kill "$a"; fi ;;
            addi)  if v=$(ppc_imm "$c"); then
                       v=$(( (v << 48) >> 48 ))
                       if [[ $b == 0 ]]; then ppc_set "$a" "$v"
                       elif vb=$(ppc_get "$b"); then ppc_set "$a" $(( vb + v )); else ppc_kill "$a"; fi
                   else ppc_kill "$a"; fi ;;
            mr)    if vb=$(ppc_get "$b"); then ppc_set "$a" "$vb"; else ppc_kill "$a"; fi ;;
            ori|oris)
                   if vb=$(ppc_get "$b") && v=$(ppc_imm "$c"); then
                       [[ $op == oris ]] && v=$(( (v & 0xffff) << 16 )) || v=$(( v & 0xffff ))
                       ppc_set "$a" $(( vb | v ))
                   else ppc_kill "$a"; fi ;;
            sldi)  if vb=$(ppc_get "$b") && v=$(ppc_imm "$c"); then ppc_set "$a" $(( vb << v )); else ppc_kill "$a"; fi ;;
            clrldi)
                   if vb=$(ppc_get "$b") && v=$(ppc_imm "$c"); then ppc_set "$a" $(( vb & $(ppc_mask "$v" 63) )); else ppc_kill "$a"; fi ;;
            rldicl)
                   if vb=$(ppc_get "$b") && v=$(ppc_imm "$c") && m=$(ppc_imm "$d"); then
                       ppc_set "$a" $(( $(ppc_rotl "$vb" "$v") & $(ppc_mask "$m" 63) ))
                   else ppc_kill "$a"; fi ;;
            rldimi)
                   if va=$(ppc_get "$a") && vb=$(ppc_get "$b") && v=$(ppc_imm "$c") && m=$(ppc_imm "$d") && (( m <= 63 - v )); then
                       local mk; mk=$(ppc_mask "$m" $(( 63 - v )))
                       ppc_set "$a" $(( ($(ppc_rotl "$vb" "$v") & mk) | (va & ~mk) ))
                   else ppc_kill "$a"; fi ;;
            # Memory: only an std to off(base) followed by an ld of the very same off(base) is tracked.  Any std first
            # forgets every slot (another base register may alias it, and offsets may overlap); any other store
            # (stw, stb, sth, vector, float, update forms) forgets them all.
            std)   PPC_M=()
                   if [[ $b =~ ^-?[0-9]+\([0-9]+\)$ ]] && va=$(ppc_get "$a"); then PPC_M[$b]=$va; fi ;;
            ld)    if [[ $b =~ ^-?[0-9]+\([0-9]+\)$ && -n "${PPC_M[$b]+x}" ]]; then ppc_set "$a" "${PPC_M[$b]}"; else ppc_kill "$a"; fi ;;
            stdu|stwu|stbu|sthu|ldu|lwzu|lbzu|lhzu)
                   PPC_M=()
                   [[ $b =~ \(([0-9]+)\)$ ]] && ppc_kill "${BASH_REMATCH[1]}"
                   [[ $op == l* ]] && ppc_kill "$a" ;;
            st*)   PPC_M=() ;;
            # Control flow and calls are not modelled: a branch, a call or a conditional return before the stop
            # line makes every result unknown (fail closed).
            b|b?|b??|b???|b????|bl|bla|blr*|bctr*|bdnz*|bdz*|bc|bca|bcl|bclr*|bcctr*)
                   PPC_UNSUPPORTED="$line" ;;
            lv*|cmp*|mtctr|mtlr|mtcrf|nop|isync|sync|lwsync|eieio|vor|vxor|vand|vperm|vsldoi|xxlor) ;;
            *)     [[ $a =~ ^[0-9]+$ ]] && ppc_kill "$a" ;;
        esac
    done <<< "$body"
}

ppc_reg() { local v; if [[ -z $PPC_UNSUPPORTED ]] && v=$(ppc_get "$1"); then printf '0x%016x\n' "$v"; else echo unknown; fi; }

ppc_ea() {
    local line=$1 op a ra rb vra vrb
    op=${line%%[[:space:]]*}
    IFS=, read -r a ra rb <<< "$(echo "${line#"$op"}" | tr -d '[:space:]')"
    [[ -z $PPC_UNSUPPORTED && $ra =~ ^[0-9]+$ && $rb =~ ^[0-9]+$ ]] || { echo unknown; return; }
    if [[ $ra == 0 ]]; then vra=0; else vra=$(ppc_get "$ra") || { echo unknown; return; }; fi
    vrb=$(ppc_get "$rb") || { echo unknown; return; }
    printf '0x%016x\n' $(( vra + vrb ))
}
