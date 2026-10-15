# ============================================================================
#  FMT 全功能冒烟测试（对着**已安装的服务**跑）
#
#  用法：
#      pwsh -File tools/smoke-test.ps1
#      pwsh -File tools/smoke-test.ps1 -Exe 'D:\Data\Temp\JMT\fmt\fmt.exe'
#
#  安全约定（很重要）：
#    * 必须用**部署目录里的那个 exe**。用构建目录的 exe 会把服务的数据库
#      切换到构建目录去（数据根跟着 exe 走），测试完你就"看不到自己的文件"了。
#    * **对着有真实数据的数据根跑，它会把真实数据搅乱**（这不是猜测，2026-10-16 实测）：
#      脚本会 `bucket use` 切桶、`file delete` 真实文件（软删除进回收站）、
#      `trash delete` 永久删掉回收站里原有条目、并往 `data/share.json` 写测试分享。
#      所以**要么用一次性的数据根**（复制一份 `config/` + `data/` + 空的 `repository/`），
#      要么跑完后按下面第 14 行的清单逐项复原（`trash list` + `trash restore` 能把软删除的
#      file 找回来；被永久删除的**找不回来**）。对着真实数据根跑时请先备份 `data/` 与 `trash/`。
#    * 所有增删都发生在自己创建的测试 Bucket 里，结束时连桶一起删掉。
#    * **不做 `trash empty` 的实操**：那会连回收站里原有的文件一起永久删除。
#      只测它的预检与拒绝路径（不带 --yes 必须 FMT-016），并确认原有条目还在。
#    * 结束时核对：原 Bucket、原文件数、原回收站条目、user.json、share.json、temp/ 全部没变。
#      **核对只是"报告"，不会自动复原**——这几项一旦不一致，需要你自己按上面的说明收拾。
# ============================================================================
param(
    [string]$Exe = 'D:\Data\Temp\JMT\fmt\fmt.exe',
    [int]$Port = 4122
)

$ErrorActionPreference = 'Stop'
$script:pass = 0
$script:fail = 0
$script:failures = @()

function Section([string]$title) {
    Write-Host ''
    Write-Host "== $title ==" -ForegroundColor Cyan
}
function Check([string]$name, [bool]$condition, [string]$detail = '') {
    if ($condition) {
        $script:pass++
        Write-Host "  [OK]   $name"
    } else {
        $script:fail++
        $script:failures += $name
        Write-Host "  [FAIL] $name   $detail" -ForegroundColor Red
    }
}
function CheckEq([string]$name, $actual, $expected) {
    Check $name ($actual -eq $expected) "actual=[$actual] expected=[$expected]"
}

# 跑一条 CLI 命令，返回合并后的文本；同时把退出码放到 $script:code
# 用 $global:FmtExe 而不是闭包捕获 $Exe：脚本作用域的变量在函数里被同名参数
# 之类的东西遮挡过，出现过「& $Exe 去执行 .\file」这种诡异错误。
function FmtRun([string[]]$Arguments) {
    $out = & $global:FmtExe @Arguments 2>&1 | Out-String
    $script:code = $LASTEXITCODE
    return $out
}
function CliCode([string[]]$Arguments) {
    $null = FmtRun $Arguments
    return $script:code
}
function Http([string]$Path, [string]$Method = 'GET', $Body = $null, [bool]$WithToken = $true, [string]$ContentType = 'application/json') {
    $headers = @{}
    if ($WithToken) { $headers['X-FMT-Token'] = $script:token }
    $params = @{
        Uri = "http://localhost:$Port$Path"; Method = $Method; Headers = $headers
        SkipHttpErrorCheck = $true
    }
    if ($null -ne $Body) { $params['Body'] = $Body; $params['ContentType'] = $ContentType }
    return Invoke-WebRequest @params
}
function Json($Response) { return ($Response.Content | ConvertFrom-Json) }

if (-not (Test-Path -LiteralPath $Exe)) { throw "找不到 exe：$Exe" }
$Exe = (Resolve-Path -LiteralPath $Exe).Path
$global:FmtExe = $Exe
$root = Split-Path -Parent $Exe
Write-Host "被测 exe：$Exe"
Write-Host "数据根　：$root"

# ---------------------------------------------------------------- 0. 基线
Section '0. 记录你的原始状态（结束时逐项核对）'
& $Exe service start | Out-Null
Start-Sleep -Milliseconds 800
$baseFiles = (FmtRun @('file', 'list')) -join "`n"
$baseTrash = (FmtRun @('trash', 'list')) -join "`n"
$baseBuckets = (FmtRun @('bucket', 'list')) -join "`n"
$baseFileCount = ([regex]::Matches($baseFiles, '(?m)^\s+\S')).Count
$baseTrashCount = ([regex]::Matches($baseTrash, '(?m)^\s+\[(文件|桶)\]')).Count
Write-Host "  当前 Bucket 文件条目：$baseFileCount ；回收站条目：$baseTrashCount"

# 1. 版本 / 帮助
Section '1. 版本与帮助'
$version = FmtRun @('version')
Check 'version 打印程序名与版本' ($version -match 'File Manager Tool') $version.Trim()
CheckEq 'version 退出码 0' (CliCode @('version')) 0
$help = FmtRun @('help')
Check 'help 列出六组命令' (($help -match '\(file\)') -and ($help -match '\(trash\)') -and ($help -match '\(share\)') -and ($help -match '\(config\)'))
$helpFile = FmtRun @('help', 'file')
Check 'help file 含 --search 与 --page-size' (($helpFile -match '--search') -and ($helpFile -match '--page-size'))
Check 'help trash 含 empty' ((FmtRun @('help', 'trash')) -match 'empty')
Check 'help config 含 max_upload_size' ((FmtRun @('help', 'config')) -match 'max_upload_size')

# 2. Bucket
Section '2. Bucket'
$testBucket = 'smoke-' + (Get-Date -Format 'MMddHHmmss')
CheckEq 'bucket create 退出码 0' (CliCode @('bucket', 'create', $testBucket)) 0
Check 'bucket list 含新桶' ((FmtRun @('bucket', 'list')) -match $testBucket)
Check 'bucket get 打到路径' ((FmtRun @('bucket', 'get', $testBucket)) -match 'repository')
CheckEq 'bucket use 退出码 0' (CliCode @('bucket', 'use', $testBucket)) 0
Check 'bucket list 标出当前桶' ((FmtRun @('bucket', 'list')) -match '\*.*' + $testBucket)

# 3. 上传与列表
Section '3. 文件上传 / 列表 / 查询'
$tmpDir = Join-Path $env:TEMP ('fmt-smoke-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $tmpDir -Force | Out-Null
$names = @('alpha.txt', 'beta.txt', 'gamma.txt')
for ($i = 0; $i -lt $names.Count; $i++) {
    [System.IO.File]::WriteAllText((Join-Path $tmpDir $names[$i]), ('content-' + $names[$i] + ('x' * (100 * ($i + 1)))))
}
foreach ($n in $names) {
    CheckEq "file upload $n" (CliCode @('file', 'upload', (Join-Path $tmpDir $n))) 0
}
$list = FmtRun @('file', 'list')
CheckEq 'file list 三条' ([regex]::Matches($list, '(?m)^\s+\S+\.txt')).Count 3
Check 'file list 显示总数' ($list -match '共 3 个文件')
$sorted = FmtRun @('file', 'list', '--sort', 'size')
Check 'file list --sort size 第一条是最大的' (($sorted -split "`n")[0] -match 'gamma')
Check 'file list --search 命中 3 条（alpha/beta/gamma 都含 .txt）' ((FmtRun @('file', 'list', '--search', '.txt')) -match '共 3 个文件')
Check 'file list --search 大小写不敏感' ((FmtRun @('file', 'list', '--search', 'ALPHA')) -match '共 1 个文件')
Check 'file list --search 无命中是 0 条' ((FmtRun @('file', 'list', '--search', 'zzzz')) -match '共 0 个文件')
$paged = FmtRun @('file', 'list', '--page', '2', '--page-size', '2')
Check 'file list 分页：第 2/2 页本页 1 条' ($paged -match '第 2/2 页，本页 1 条')
CheckEq 'file list 非法 page-size 退出码 2' (CliCode @('file', 'list', '--page-size', '99999')) 2
$detail = FmtRun @('file', 'get', 'alpha.txt')
Check 'file get 按名字查到 file_id' ($detail -match 'fmt-\d{8}-\d+')
$alphaId = ([regex]::Match($detail, 'fmt-\d{8}-\d+')).Value
Check 'file get 按 file_id 也能查' ((FmtRun @('file', 'get', $alphaId)) -match 'alpha.txt')

# 4. 校验类错误
Section '4. 校验与错误码'
$clashDir = Join-Path $tmpDir 'clash'
New-Item -ItemType Directory -Path $clashDir -Force | Out-Null
[System.IO.File]::WriteAllText((Join-Path $clashDir 'alpha.txt'), 'DIFFERENT-CONTENT-so-md5-differs')
$clash = FmtRun @('file', 'upload', (Join-Path $clashDir 'alpha.txt'))
CheckEq '内容不同 + 同名被拒（FMT-105，退出码 4）' $script:code 4
Check '  报的是 FMT-105' ($clash -match 'FMT-105')
Check '  并提示修改文件名' ($clash -match '修改文件名')
# 桶内判重（本次改动（未提交））：**先看内容、再看名字**
#   内容相同（不管名字同不同）→ FMT-304「文件已存在」，不提改名
#   内容不同 + 名字相同       → FMT-105「文件名重复」，要提改名
#   内容不同 + 名字不同       → 正常入库
$sameContentOtherName = FmtRun @('file', 'upload', (Join-Path $tmpDir 'alpha.txt'), 'alpha-dup.txt')
CheckEq '内容相同 + 换名字被拒（FMT-304，退出码 4）' $script:code 4
Check '  报的是 FMT-304「文件已存在」' ($sameContentOtherName -match 'FMT-304.*文件已存在')
Check '  **不**提示修改文件名' ($sameContentOtherName -notmatch '修改文件名')
$sameContentSameName = FmtRun @('file', 'upload', (Join-Path $tmpDir 'alpha.txt'))
CheckEq '内容相同 + 同名字被拒（FMT-304，退出码 4）' $script:code 4
Check '  也**不**提示修改文件名' ($sameContentSameName -notmatch '修改文件名')
Check '内容不同的新名字可以正常入库' (CliCode @('file', 'upload', (Join-Path $tmpDir 'gamma.txt'), 'brand-new.txt')) 0
Check '  列表里能看到新文件' ((FmtRun @('file', 'list')) -match 'brand-new\.txt')
$null = FmtRun @('file', 'delete', 'brand-new.txt', '--yes'); $null = FmtRun @('trash', 'delete', 'brand-new.txt', '--yes')
# 另一个桶里可以有同名 + 同内容：切过去传，两边都成功
$otherBucket = $testBucket + '-b'
CheckEq '新建第二个桶' (CliCode @('bucket', 'create', $otherBucket)) 0
CheckEq '切到第二个桶' (CliCode @('bucket', 'use', $otherBucket)) 0
CheckEq '同内容 + 同名在另一个桶可以入库（退出码 0）' (CliCode @('file', 'upload', (Join-Path $tmpDir 'alpha.txt'), 'alpha.txt')) 0
Check '  第二个桶里能看到它' ((FmtRun @('file', 'list')) -match '共 1 个文件')
CheckEq '切回原桶' (CliCode @('bucket', 'use', $testBucket)) 0
Check '原桶的 alpha.txt 还在' ((FmtRun @('file', 'list')) -match 'alpha\.txt')
CheckEq '不存在的文件（FMT-002）' (CliCode @('file', 'upload', (Join-Path $tmpDir 'nope.bin'))) 3
CheckEq '与 file_id 同形的名字（FMT-106）' (CliCode @('file', 'upload', (Join-Path $tmpDir 'beta.txt'), 'fmt-20260101-0')) 2
CheckEq '查不到的文件退出码 3' (CliCode @('file', 'get', 'not-here.txt')) 3
CheckEq '未知子命令当前是 FMT-602（退出码 8，见报告发现）' (CliCode @('file', 'frobnicate')) 8
CheckEq '未知模块报 FMT-602（退出码 8）' (CliCode @('server', 'nosuch')) 8
Check '未知模块提示未实现' ((FmtRun @('server', 'nosuch')) -match 'FMT-602')

# 5. 上传上限
Section '5. 上传大小上限（config）'
$cfg = FmtRun @('config', 'list')
Check 'config list 含上传上限' ($cfg -match '上传上限')
Check 'config list 含当前用户（统一风格）' ($cfg -match '当前用户：')
Check 'config list 含当前 Bucket（统一风格）' ($cfg -match '当前 Bucket：')
# 大小单位与语言必须**各占一行**（以前挤在同一行）
$unitLine = (($cfg -split "`n") | Select-String -SimpleMatch '大小单位：' | Select-Object -First 1).Line
Check '大小单位这一行只放一个字段' ($null -ne $unitLine -and $unitLine -notmatch '语言')
# Select-String 返回的是 MatchInfo 对象，Check 的第二个参数是 [bool] → 必须显式取布尔，
# 否则 PowerShell 会抛「无法将 MatchInfo 转换为 Boolean」把整轮冒烟打断（真机踩过）。
$langLine = ($cfg -split "`n") | Select-String -SimpleMatch '语言：' | Select-Object -First 1
Check '语言单独一行' ($null -ne $langLine)
# 两个配置文件按顺序：config.json 在前、server.json 在后
$cfgAt = $cfg.IndexOf('配置文件：config/config.json')
$srvAt = $cfg.IndexOf('网络服务配置：config/server.json')
Check '两个配置文件按顺序输出（config.json 在前）' ($cfgAt -ge 0 -and $srvAt -gt $cfgAt)
# 本次改动（未提交）：config list 也输出 config/server.json 的公共字段
Check 'config list 含网络服务 enabled 一行' ($cfg -match '网络服务 enabled：(true|false)')
Check 'config list 含服务地址 host 一行' ($cfg -match '服务地址 host：')
Check 'config list 含服务端口 port 一行' ($cfg -match '服务端口 port：\d+')
# token **默认不打印**（会暴露在屏幕/日志/工单/截图里）
Check 'config list 默认不打印 token' ($cfg -notmatch '访问 token：[0-9a-f]{32}')
Check '  且说明怎么显示它' ($cfg -match '已隐藏')
# 注意：**不在这里动 server_enabled / server_host / server_port**——那会触发
# 「改完自动重启服务」（提权重装），冒烟脚本不该去动用户的真实服务。
CheckEq '非法布尔被拒（FMT-001，退出码 2）' (CliCode @('config', 'set', 'server_enabled', 'maybe')) 2
CheckEq '非法端口被拒（FMT-001，退出码 2）' (CliCode @('config', 'set', 'server_port', '70000')) 2
# 要 token 得显式加 --show-token（这里只用它取 token，不改任何配置）
$cfgToken = FmtRun @('config', 'list', '--show-token')
$script:token = ([regex]::Match($cfgToken, '访问 token：([0-9a-f]{32})')).Groups[1].Value
Check '--show-token 才给出 token' ($script:token.Length -eq 32)
CheckEq 'config set 1MB' (CliCode @('config', 'set', 'max_upload_size', '1MB')) 0
Check 'config list 反映 1048576' ((FmtRun @('config', 'list')) -match '1048576')
$big = Join-Path $tmpDir 'big.bin'
[System.IO.File]::WriteAllBytes($big, (New-Object byte[] (2MB)))
CheckEq '超过上限的上传被拒（FMT-303，退出码 2）' (CliCode @('file', 'upload', $big)) 2
CheckEq '只读配置项被拒（退出码 2）' (CliCode @('config', 'set', 'current_user', 'x')) 2
CheckEq '非法大小被拒（退出码 2）' (CliCode @('config', 'set', 'max_upload_size', '0')) 2
CheckEq '恢复上限 50MB' (CliCode @('config', 'set', 'max_upload_size', '50MB')) 0

# 6. 软删除 / 回收站
Section '6. 软删除与回收站'
CheckEq 'file delete 软删除' (CliCode @('file', 'delete', 'alpha.txt')) 0
Check 'file list 少了一条' ((FmtRun @('file', 'list')) -match '共 2 个文件')
Check 'trash list 出现该文件' ((FmtRun @('trash', 'list')) -match 'alpha.txt')
CheckEq 'trash get 退出码 0' (CliCode @('trash', 'get', $alphaId)) 0
CheckEq 'trash restore 恢复' (CliCode @('trash', 'restore', $alphaId)) 0
Check 'file list 又回到 3 条' ((FmtRun @('file', 'list')) -match '共 3 个文件')
CheckEq 'trash empty 不带 --yes 必须 FMT-016（退出码 2）' (CliCode @('trash', 'empty')) 2
Check 'trash empty 预检说明了不可恢复' ((FmtRun @('trash', 'empty')) -match '不可恢复')
Check '回收站里原有条目**没被删掉**' ((FmtRun @('trash', 'list')) -match 'a7\.jpg')

# 7. 分享
Section '7. 分享'
$created = FmtRun @('share', 'create', $alphaId)
$shareId = ([regex]::Match($created, '(?m)^share_id：([0-9a-f]{12})')).Groups[1].Value
CheckEq 'share create 返回 12 位 share_id' ($shareId.Length) 12
Check 'share create 默认 20 次' ($created -match '下载次数：0/20')
Check 'share create 默认 7 天到期' ($created -match '到期：\d{4}-\d{2}-\d{2}T')
Check 'share get 状态可用' ((FmtRun @('share', 'get', $shareId)) -match '状态：可用')
Check 'share list 一条' ((FmtRun @('share', 'list', $alphaId)) -match '共 1 条分享')
# 完整分享链接（本次改动（未提交））：无论 HTTP 服务有没有开都给
$shareLink = 'http://localhost:4122/api/share/' + $shareId + '/download'
Check 'share create 打印完整分享链接' ($created -match [regex]::Escape("分享链接：$shareLink"))
Check 'share get 也带完整分享链接' ((FmtRun @('share', 'get', $shareId)) -match [regex]::Escape("分享链接：$shareLink"))
# 按**文件名**创建分享（本次改动（未提交）：与 file get / file delete 同一套定位规则）
$byName = FmtRun @('share', 'create', 'alpha.txt')
CheckEq 'share create 收文件名' $script:code 0
Check '  按文件名创建也回链接' ($byName -match 'api/share/[0-9a-f]{12}/download')
$byNameId = ([regex]::Match($byName, '(?m)^share_id：([0-9a-f]{12})')).Groups[1].Value
CheckEq '  名字不存在 → FMT-002（退出码 3）' (CliCode @('share', 'create', 'nope.txt')) 3
$null = FmtRun @('share', 'delete', $byNameId)
# file get 的预览 / 下载链接（本次改动（未提交））
$detail2 = FmtRun @('file', 'get', $alphaId)
Check 'file get 打印预览链接' ($detail2 -match [regex]::Escape("预览链接：http://localhost:4122/api/file/$alphaId/preview"))
Check 'file get 打印下载链接' ($detail2 -match [regex]::Escape("下载链接：http://localhost:4122/api/file/$alphaId/download"))
CheckEq 'share download 记账' (CliCode @('share', 'download', $shareId)) 0
Check '计数变成 1/20' ((FmtRun @('share', 'get', $shareId)) -match '1/20')
CheckEq '未知 share get 退出码 3' (CliCode @('share', 'get', '000000000000')) 3

# 过期：直接改 share.json 写成过去的时间（只动我们自己的那条）
$sharePath = Join-Path $root 'data\share.json'
$shareJson = Get-Content -LiteralPath $sharePath -Encoding UTF8 -Raw | ConvertFrom-Json
$before = $shareJson.shares.Count
foreach ($s in $shareJson.shares) { if ($s.share_id -eq $shareId) { $s.expire_time = '2000-01-01T00:00:00' } }
($shareJson | ConvertTo-Json -Depth 8) | Set-Content -LiteralPath $sharePath -Encoding UTF8
Start-Sleep -Milliseconds 300
Check '过期后 get 如实报「已过期」' ((FmtRun @('share', 'get', $shareId)) -match '已过期')
CheckEq '过期后 download 被拒（FMT-501，退出码 5）' (CliCode @('share', 'download', $shareId)) 5
CheckEq 'share delete 撤销' (CliCode @('share', 'delete', $shareId)) 0
CheckEq '撤销后再 get 是 FMT-500（退出码 3）' (CliCode @('share', 'get', $shareId)) 3

# 次数上限：新建一条，下满 20 次，第 21 次必须被拒
$limitId = ([regex]::Match((FmtRun @('share', 'create', $alphaId)), '[0-9a-f]{12}')).Value
for ($i = 0; $i -lt 20; $i++) { $null = FmtRun @('share', 'download', $limitId) }
Check '下载满 20 次' ((FmtRun @('share', 'get', $limitId)) -match '20/20')
CheckEq '第 21 次被拒（FMT-502，退出码 5）' (CliCode @('share', 'download', $limitId)) 5
$null = FmtRun @('share', 'delete', $limitId)

# 8. HTTP
Section '8. HTTP 接口'
$ping = Http '/api/ping' -WithToken $false
CheckEq '/api/ping 不要 token（200）' $ping.StatusCode 200
$noAuth = Http '/api/file' -WithToken $false
CheckEq '不带 token 的管理接口 401' $noAuth.StatusCode 401
CheckEq '错误码是 FMT-018' ((Json $noAuth).error.code) 'FMT-018'
$bearer = Invoke-WebRequest -Uri "http://localhost:$Port/api/status" -Headers @{ Authorization = "Bearer $script:token" } -SkipHttpErrorCheck
CheckEq 'Authorization: Bearer 也认（200）' $bearer.StatusCode 200
# Bucket：只读列表回到 HTTP 面，写接口仍然故意不做
$bucketList = Json (Http '/api/bucket')
CheckEq 'GET /api/bucket 只读列表（200）' $bucketList.ok $true
Check '  带 buckets 与 current_bucket' (($null -ne $bucketList.data.buckets) -and ($null -ne $bucketList.data.current_bucket))
$bucketWrite = Http '/api/bucket/x'
CheckEq '桶的写接口仍然没有（404 + FMT-017）' $bucketWrite.StatusCode 404
CheckEq '  错误码 FMT-017' ((Json $bucketWrite).error.code) 'FMT-017'

# 配置：只读，而且**任何情况下都不回传 token**（连 ?include_token=1 这种开关都不提供）
$cfg = Json (Http '/api/config')
CheckEq 'GET /api/config 200（只读）' $cfg.ok $true
Check '  默认不返回 token' ($null -eq $cfg.data.token)
Check '  带 max_upload_size' ($cfg.data.max_upload_size -gt 0)
Check '  塞 ?include_token=1 也不给 token' ($null -eq (Json (Http '/api/config?include_token=1')).data.token)

# 清空回收站：**只测预检与拒绝路径**——真删会把回收站里原有条目一起清掉（见脚本头部约定）
$trashBefore = (Json (Http '/api/trash')).data.count
$emptyPlan = Json (Http '/api/trash/empty?dry_run=1' -Method DELETE)
CheckEq 'DELETE /api/trash/empty?dry_run=1 预检 200' $emptyPlan.ok $true
Check '  预检的 needs_confirm 与条目数自洽' ($emptyPlan.data.needs_confirm -eq (($emptyPlan.data.files + $emptyPlan.data.buckets) -gt 0))
if ($emptyPlan.data.needs_confirm) {
    $refused = Http '/api/trash/empty' -Method DELETE
    CheckEq '有内容不给 force → 400 + FMT-016' $refused.StatusCode 400
    CheckEq '  错误码 FMT-016' ((Json $refused).error.code) 'FMT-016'
} else {
    CheckEq '回收站为空：不带 force 也直接成功（不为空操作要确认）' (Http '/api/trash/empty' -Method DELETE).StatusCode 200
}
CheckEq '预检与拒绝都没动数据（回收站条目数不变）' (Json (Http '/api/trash')).data.count $trashBefore
$unknown = Http '/api/nosuchthing'
CheckEq '完全未知路径 404 + FMT-017' $unknown.StatusCode 404
$fileList = Json (Http "/api/file?search=alpha&page=1&page_size=2")
CheckEq '/api/file 搜索命中 1' $fileList.data.total 1
CheckEq '/api/file 回显 page_size' $fileList.data.page_size 2
$badPage = Http '/api/file?page_size=abc'
CheckEq 'page_size 非数字 → 400' $badPage.StatusCode 400
CheckEq '  错误码 FMT-001' ((Json $badPage).error.code) 'FMT-001'

$body = [System.Text.Encoding]::UTF8.GetBytes('http-stream-body-0123456789')
$up = Http '/api/file/upload?name=http-probe.txt' -Method POST -Body $body -ContentType 'application/octet-stream'
CheckEq '流式上传（请求体即内容）200' $up.StatusCode 200
$probeId = ([regex]::Match($up.Content, 'fmt-\d{8}-\d+')).Value
$dlPath = Join-Path $tmpDir 'http-probe-out.txt'
Invoke-WebRequest -Uri "http://localhost:$Port/api/file/$probeId/download" -Headers @{ 'X-FMT-Token' = $script:token } -OutFile $dlPath -SkipHttpErrorCheck | Out-Null
Check '下载字节与原内容一致' ([System.IO.File]::ReadAllText($dlPath) -eq 'http-stream-body-0123456789')
$preview = Http "/api/file/$probeId/preview"
CheckEq '文本预览 200' $preview.StatusCode 200
Check '  是 inline 且 text/plain' (($preview.Headers['Content-Disposition'] -match 'inline') -and ($preview.Headers['Content-Type'] -match 'text/plain'))
$binFile = Join-Path $tmpDir 'probe.bin'
[System.IO.File]::WriteAllBytes($binFile, (New-Object byte[] 32))
$upBin = Http '/api/file/upload?name=probe.bin' -Method POST -Body ([System.IO.File]::ReadAllBytes($binFile)) -ContentType 'application/octet-stream'
$binId = ([regex]::Match($upBin.Content, 'fmt-\d{8}-\d+')).Value
CheckEq '二进制预览被拒（FMT-701 → 400）' (Http "/api/file/$binId/preview").StatusCode 400

# multipart/form-data：Postman 默认的 form-data、浏览器表单、Spring 的 MultipartFile 那个形状。
# 用系统自带的 curl.exe（Invoke-WebRequest 要自己算 boundary 与 part 头，太长）。
# 口径：取**第一个带 filename 的 part**，普通文本字段读掉但不落盘；没有文件 part → FMT-100。
$mpId = ''
if (Get-Command curl.exe -ErrorAction SilentlyContinue) {
    $mpFile = Join-Path $tmpDir 'multipart-probe.txt'
    Set-Content -LiteralPath $mpFile -Value 'multipart-body-0123456789' -NoNewline -Encoding utf8
    $mpRaw = (& curl.exe -s -X POST "http://localhost:$Port/api/file/upload" `
            -H "X-FMT-Token: $($script:token)" `
            -F "file=@$mpFile" -F 'note=这个普通字段不该进文件') | Out-String
    $mp = $mpRaw | ConvertFrom-Json
    CheckEq 'multipart 上传 200' $mp.ok $true
    CheckEq '  part 的 filename 就是落库的名字' $mp.data.file_name 'multipart-probe.txt'
    CheckEq '  普通字段不落盘（大小就是文件大小）' $mp.data.size 27
    $mpId = $mp.data.file_id
    $mpOut = Join-Path $tmpDir 'multipart-out.txt'
    Invoke-WebRequest -Uri "http://localhost:$Port/api/file/$mpId/download" -Headers @{ 'X-FMT-Token' = $script:token } -OutFile $mpOut -SkipHttpErrorCheck | Out-Null
    Check '  下载字节与原内容一致' ([System.IO.File]::ReadAllText($mpOut) -eq 'multipart-body-0123456789')
    $onlyText = ((& curl.exe -s -X POST "http://localhost:$Port/api/file/upload" `
                -H "X-FMT-Token: $($script:token)" -F 'note=只有文本字段') | Out-String) | ConvertFrom-Json
    CheckEq '只有普通字段（没有文件 part）→ FMT-100' $onlyText.error.code 'FMT-100'
} else {
    Write-Host '  [SKIP] 没找到 curl.exe：跳过 multipart 探针' -ForegroundColor Yellow
}

# 上限：HTTP 层也吃 config 里的那个值，而且是**每次请求现取**——`config set` 之后
# 不用重启服务。超限时应该在**收的过程中**就中止：消息是 HTTP 层那句
# 「请求体超过上传上限（已收 …）」，而不是收完再被业务层拒的「文件超过上传上限（… > … 字节）」。
CheckEq '把上限降到 1KB' (CliCode @('config', 'set', 'max_upload_size', '1KB')) 0
$overBody = New-Object byte[] 4096
$overResp = Http '/api/file/upload?name=http-over-limit.bin' -Method POST -Body $overBody -ContentType 'application/octet-stream'
CheckEq 'HTTP 超限 → 400' $overResp.StatusCode 400
$overJson = Json $overResp
CheckEq '  错误码 FMT-303' $overJson.error.code 'FMT-303'
Check '  消息来自 HTTP 层（收的过程中就中止，不是收完再拒）' ($overJson.error.message -match '请求体超过上传上限')
CheckEq '恢复上限 50MB' (CliCode @('config', 'set', 'max_upload_size', '50MB')) 0
$trashHttp = Json (Http '/api/trash')
Check 'GET /api/trash 可达且有 entries' ($null -ne $trashHttp.data.entries)
$shareHttp = Json (Http '/api/share' -Method POST -Body (@{ file_id = $alphaId } | ConvertTo-Json) -ContentType 'application/json')
$httpShareId = $shareHttp.data.share_id
CheckEq 'POST /api/share 建分享' $httpShareId.Length 12
$pub = Invoke-WebRequest -Uri "http://localhost:$Port/api/share/$httpShareId/download" -SkipHttpErrorCheck
CheckEq '公开分享下载**不带 token** 200' $pub.StatusCode 200
$pubText = if ($pub.Content -is [byte[]]) { [System.Text.Encoding]::UTF8.GetString($pub.Content) } else { [string]$pub.Content }
Check '  下载内容与 alpha.txt 逐字节一致' ($pubText -eq [System.IO.File]::ReadAllText((Join-Path $tmpDir 'alpha.txt')))
Check '  计数变成 1' ((Json (Http "/api/share/$httpShareId")).data.download_count -eq 1)
$null = Http "/api/share/$httpShareId" -Method DELETE
CheckEq 'DELETE /api/share 撤销' ((Json (Http "/api/share/$httpShareId")).error.code) 'FMT-500'

# 9. 清理
Section '9. 清理测试数据'
foreach ($id in @($probeId, $binId, $mpId) | Where-Object { -not [string]::IsNullOrEmpty($_) }) {
    $null = FmtRun @('file', 'delete', $id, '--yes')
    $null = FmtRun @('trash', 'delete', $id, '--yes')
}
foreach ($n in @($names)) {
    $null = FmtRun @('file', 'delete', $n, '--yes')
    $null = FmtRun @('trash', 'delete', $n, '--yes')
}
# 第二个桶里那份同名 + 同内容：整个桶软删除再永久删除
CheckEq '清理：删掉第二个桶' (CliCode @('bucket', 'delete', $otherBucket, '--yes')) 0
$otherTrash = ((FmtRun @('trash', 'list')) -split "`n") |
    Select-String ([regex]::Escape($otherBucket)) | Select-Object -First 1
$otherTrashName = ([regex]::Match($otherTrash.Line, '->\s+(\S+)')).Groups[1].Value
CheckEq '清理：永久删除第二个桶的回收站条目' (CliCode @('trash', 'delete', $otherTrashName, '--yes')) 0
$null = FmtRun @('bucket', 'use', 'lazy')
$null = FmtRun @('bucket', 'delete', $testBucket, '--yes')   # 这一步是**软删除**（进回收站）
$trashLine = ((FmtRun @('trash', 'list')) -split "`n") |
    Select-String ([regex]::Escape($testBucket)) | Select-Object -First 1
$trashName = ([regex]::Match($trashLine.Line, '->\s+(\S+)')).Groups[1].Value
Check '测试 Bucket 进了回收站' (-not [string]::IsNullOrEmpty($trashName)) "trashName=[$trashName]"
CheckEq '再永久删除该桶（trash delete）' (CliCode @('trash', 'delete', $trashName, '--yes')) 0

# 10. 核对原状
Section '10. 核对你的原始数据没变'
$nowFiles = (FmtRun @('file', 'list')) -join "`n"
$nowTrash = (FmtRun @('trash', 'list')) -join "`n"
$nowBuckets = (FmtRun @('bucket', 'list')) -join "`n"
$nowFileCount = ([regex]::Matches($nowFiles, '(?m)^\s+\S')).Count
$nowTrashCount = ([regex]::Matches($nowTrash, '(?m)^\s+\[(文件|桶)\]')).Count
CheckEq '当前 Bucket 文件数一致' $nowFileCount $baseFileCount
CheckEq '回收站条目数一致' $nowTrashCount $baseTrashCount
CheckEq '原文件 asdva.jpg 还在' ($nowFiles -match 'asdva\.jpg') $true
CheckEq '原文件 17364485.jpg 还在' ($nowFiles -match '17364485\.jpg') $true
CheckEq '原回收站条目 a7.jpg 还在' ($nowTrash -match 'a7\.jpg') $true
CheckEq '测试 Bucket 已删除' ($nowBuckets -match $testBucket) $false
$userJson = Get-Content -LiteralPath (Join-Path $root 'data\user.json') -Encoding UTF8 -Raw | ConvertFrom-Json
CheckEq 'user.json 仍是单用户形状（没有 users 数组）' ($null -eq $userJson.users) $true
CheckEq 'user.json 仍带着账号（user_id 非空）' (-not [string]::IsNullOrEmpty($userJson.user_id)) $true
CheckEq 'user.json 仍无 buckets 字段' ($null -eq $userJson.buckets) $true
$shareAfter = Get-Content -LiteralPath $sharePath -Encoding UTF8 -Raw | ConvertFrom-Json
CheckEq 'share.json 没留下测试分享' $shareAfter.shares.Count 0
$leftovers = Get-ChildItem (Join-Path $root 'temp') -Filter 'fmt-upload-*' -ErrorAction SilentlyContinue
CheckEq 'temp/ 无残留暂存文件' $leftovers.Count 0
CheckEq '上传上限已恢复 50MB' ((FmtRun @('config', 'list')) -match '上传上限：50MB') $true
Remove-Item $tmpDir -Recurse -Force

# 汇总
Write-Host ''
Write-Host ('=' * 60)
Write-Host ("结果：通过 $script:pass ，失败 $script:fail") -ForegroundColor ($(if ($script:fail -eq 0) { 'Green' } else { 'Red' }))
if ($script:fail -gt 0) {
    Write-Host '失败项：' -ForegroundColor Red
    $script:failures | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
    exit 1
}
exit 0
