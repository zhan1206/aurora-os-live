import urllib.request, json, os

url = "https://api.github.com/repos/copy/v86/releases/latest"
req = urllib.request.Request(url, headers={"Accept": "application/json"})
data = json.loads(urllib.request.urlopen(req).read())

dest = os.path.join(os.path.dirname(__file__), "..", "js", "v86")
os.makedirs(dest, exist_ok=True)

for asset in data["assets"]:
    name = asset["name"]
    dl_url = asset["browser_download_url"]
    filepath = os.path.join(dest, name)
    if os.path.exists(filepath):
        print(f"SKIP (exists): {name}")
        continue
    print(f"DOWNLOAD: {name} ...")
    urllib.request.urlretrieve(dl_url, filepath)
    print(f"  -> {filepath}")

print("Done!")
