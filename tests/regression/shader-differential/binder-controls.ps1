# Deterministic controls for the FP auto binder. No corpus paths or private
# containers belong here. Compile both sides with the same compiler; when the
# reference compiler is supplied the stager uses it for this instrument check.
function New-BinderControls([string]$Scratch, [string]$Controls, [scriptblock]$Compile) {
    New-Item -ItemType Directory -Force -Path $Scratch | Out-Null
    $rows = New-Object 'System.Collections.Generic.List[string]'
    function Add-BinderPair([string]$Name, [string]$Role, [string]$A, [string]$B, [string]$Uniform = '0') {
        foreach ($side in @('a', 'b')) {
            $source = Join-Path $Scratch ($Name + '_' + $side + '.cg')
            $dest = Join-Path $Controls ($Name + '_' + $side + '.fpo')
            $body = if ($side -eq 'a') { $A } else { $B }
            Set-Content -LiteralPath $source -Value $body -Encoding Ascii
            $null = & $Compile $source $dest
        }
        $rows.Add("B|$Role|$Name|controls/${Name}_a.fpo|controls/${Name}_b.fpo|$Uniform")
    }
    function Shader([string]$Body, [string]$Uniform = '') {
        $arg = if ($Uniform) { ', ' + $Uniform } else { '' }
        return "void main(float4 t:TEXCOORD0, out float4 color:COLOR$arg){$Body}"
    }
    function Permuted([string]$Name, [string[]]$Values) {
        $order = @('RGBA','RGAB','RBGA','RBAG','RAGB','RABG','GRBA','GRAB','GBRA','GBAR','GARB','GABR','BRGA','BRAG','BGRA','BGAR','BARG','BAGR','ARGB','ARBG','AGRB','AGBR','ABRG','ABGR')
        $p = [int]((Fnv1a $Name) % 24)
        $v = foreach ($c in $order[$p].ToCharArray()) { $Values['RGBA'.IndexOf($c)] }
        return ($v -join ',')
    }
    # Rectangle texel coordinates, then a half-image wrapped offset.
    foreach ($i in 0..1) {
        $coord = if ($i) { 't.xy*64.0 - 32.0' } else { 't.xy*64.0' }
        $xy = if ($i) { 'frac(t.xy-0.5)*64.0' } else { 't.xy*64.0' }
        $v = Permuted 'u_rect' @('4*x','4*(63-y)','4*(63-x)','4*y')
        Add-BinderPair "binder_rect_$i" 'control-binder-rect' (Shader "color=texRECT(u_rect,$coord);" 'uniform samplerRECT u_rect') (Shader "float2 q=floor($xy);float x=q.x,y=q.y;color=float4($v)/255.0;")
    }
    # Six face orientations. Pixel centers stay away from face boundaries.
    $directions = @('float3(1,-q.y,-q.x)','float3(-1,-q.y,q.x)','float3(q.x,1,q.y)','float3(q.x,-1,-q.y)','float3(q.x,-q.y,1)','float3(-q.x,-q.y,-1)')
    foreach ($i in 0..5) {
        $v = Permuted 'u_cube' @('4*x','4*(63-y)',[string](16+28*$i),'4*y')
        Add-BinderPair "binder_cube_$i" 'control-binder-cube' (Shader "float2 q=t.xy*2-1;color=texCUBE(u_cube,$($directions[$i]));" 'uniform samplerCUBE u_cube') (Shader "float x=floor(t.x*64),y=floor(t.y*64);color=float4($v)/255.0;")
    }
    foreach ($i in 0..7) {
        $z = ([decimal](2*$i+1)/16).ToString([Globalization.CultureInfo]::InvariantCulture)
        $v = Permuted 'u_volume' @('4*x','4*(63-y)',[string](16+28*$i),'4*y')
        Add-BinderPair "binder_volume_$i" 'control-binder-volume' (Shader "color=tex3D(u_volume,float3(t.xy,$z));" 'uniform sampler3D u_volume') (Shader "float x=floor(t.x*64),y=floor(t.y*64);color=float4($v)/255.0;")
    }
    # Square and both rectangular orientations: every component contributes.
    $shapes = @(@(3,3),@(4,4),@(3,4),@(4,3))
    foreach ($i in 0..3) {
        $r,$c = $shapes[$i]
        $type = "float${r}x${c}"
        $matrix = Auto-Matrix 'u_matrix'
        $vals = for ($y=0; $y -lt $r; $y++) { for ($x=0; $x -lt $c; $x++) { $matrix[4*$y+$x] } }
        $vectorExpr = if ($c -eq 3) { 'float3(t.xy,.5)' } else { 'float4(t.xy,.5,1)' }
        $result = "mul(u_matrix,$vectorExpr)"
        if ($r -eq 3) { $result = "float4($result,1)" }
        Add-BinderPair "binder_matrix_$i" 'control-binder-matrix' (Shader "color=$result;" "uniform $type u_matrix") (Shader "$type u_matrix=$type($($vals -join ','));color=$result;") 'auto'
    }
    # Existing half paths remain checked independently of the new matrix set.
    foreach ($i in 0..1) {
        $width = if ($i) { 4 } else { 1 }
        $type = if ($i) { 'half4' } else { 'half' }
        $values = foreach ($k in 0..($width-1)) { Auto-Value 'u_half' $k }
        $result = if ($i) { 'u_half' } else { 'float4(u_half)' }
        Add-BinderPair "binder_half_$i" 'control-auto' (Shader "color=$result;" "uniform $type u_half") (Shader "$type u_half=$type($($values -join ','));color=$result;") 'auto'
    }
    return ,$rows.ToArray()
}
