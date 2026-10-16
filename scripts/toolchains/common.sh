# Shared by the setup_*.sh scripts. Everything goes under $TOOLCHAINS
# (default ~/toolchains); nothing needs root except the devkitPro step.
set -euo pipefail
TOOLCHAINS="${TOOLCHAINS:-$HOME/toolchains}"
mkdir -p "$TOOLCHAINS"
gh_latest_asset() {   # repo, regex -> download URL of the newest matching asset
  # In CI, the run's own token avoids the anonymous API rate limit.
  curl -fsSL ${GITHUB_TOKEN:+-H "Authorization: Bearer $GITHUB_TOKEN"} \
    "https://api.github.com/repos/$1/releases/latest" |
    python3 -c "import sys,json,re;d=json.load(sys.stdin);print(next(a['browser_download_url'] for a in d['assets'] if re.search(r'''$2''',a['name'])))"
}
