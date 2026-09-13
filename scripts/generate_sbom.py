#!/usr/bin/env python3
"""Writes CycloneDX 1.6 and SPDX 2.3 SBOMs for SCD3D11.dll.

SBOM scanners only understand package managers, so they cannot see this project's vendored
sources. The components are therefore declared here; keep DEPENDENCIES in sync with vendor/.
Both formats are written directly: the CycloneDX CLI's SPDX conversion drops the main package
and relationships and produces invalid SPDX identifiers. CI validates both files.

Usage: generate_sbom.py VERSION DLL_PATH OUTPUT_DIRECTORY
"""

import hashlib
import json
import os
import subprocess
import sys
import uuid
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def git(*args):
    return subprocess.run(["git", *args], cwd=ROOT, check=True, capture_output=True, text=True).stdout.strip()


def remote_tag(url, commit):
    # Submodules are checked out shallow in CI, so look the tag up on the remote instead of locally.
    for line in git("ls-remote", "--tags", url).splitlines():
        sha, ref = line.split()
        if sha == commit:
            return ref.removeprefix("refs/tags/").removesuffix("^{}")
    return None


def cyclonedx_component(package):
    component = {"type": "library", "bom-ref": package["purl"], "name": package["name"]}
    if package.get("version"):
        component["version"] = package["version"]
    component["description"] = package["description"]
    if package.get("sha256"):
        component["hashes"] = [{"alg": "SHA-256", "content": package["sha256"]}]
    component["licenses"] = [{"license": {"id": package["license"]}}]
    component["purl"] = package["purl"]
    component["externalReferences"] = [{"type": "vcs", "url": package["vcs"]}]
    return component


def spdx_package(package):
    result = {"SPDXID": package["spdx_id"], "name": package["name"]}
    if package.get("version"):
        result["versionInfo"] = package["version"]
    result["description"] = package["description"]
    result["downloadLocation"] = package["download_location"]
    result["filesAnalyzed"] = False
    if package.get("sha256"):
        result["checksums"] = [{"algorithm": "SHA256", "checksumValue": package["sha256"]}]
    result["licenseConcluded"] = "NOASSERTION"
    result["licenseDeclared"] = package["license"]
    result["copyrightText"] = "NOASSERTION"
    result["primaryPackagePurpose"] = "LIBRARY"
    result["externalRefs"] = [
        {"referenceCategory": "PACKAGE-MANAGER", "referenceType": "purl", "referenceLocator": package["purl"]}
    ]
    return result


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    version = sys.argv[1].removeprefix("v")
    dll_path = Path(sys.argv[2])
    output_directory = Path(sys.argv[3])

    repository = os.environ.get("GITHUB_REPOSITORY", "caspervg/scd3d11")
    timestamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    serial = uuid.uuid4()

    reshade_url = "https://github.com/crosire/reshade"
    reshade_commit = git("-C", "vendor/reshade", "rev-parse", "HEAD")
    reshade_revision = remote_tag(f"{reshade_url}.git", reshade_commit) or reshade_commit

    main_package = {
        "spdx_id": "SPDXRef-SCD3D11",
        "name": "SCD3D11",
        "version": version,
        "description": "Direct3D 11 driver for SimCity 4",
        "license": "LGPL-2.1-or-later",
        "purl": f"pkg:github/{repository}@v{version}",
        "vcs": f"https://github.com/{repository}",
        "download_location": f"git+https://github.com/{repository}.git@{git('rev-parse', 'HEAD')}",
        "sha256": hashlib.sha256(dll_path.read_bytes()).hexdigest(),
    }
    dependencies = [
        {
            "spdx_id": "SPDXRef-reshade",
            "name": "reshade",
            "version": reshade_revision.removeprefix("v"),
            "description": f"ReShade add-on API headers (include/ only), commit {reshade_commit}",
            "license": "BSD-3-Clause",
            "purl": f"pkg:github/crosire/reshade@{reshade_revision}",
            "vcs": reshade_url,
            "download_location": f"git+{reshade_url}.git@{reshade_commit}",
        },
        {
            "spdx_id": "SPDXRef-scion",
            "name": "scion",
            "version": None,  # vendored copy without a recorded upstream revision
            "description": "GZCOM framework sources from src/framework, vendored in vendor/framework",
            "license": "LGPL-2.1-only",
            "purl": "pkg:github/nsgomez/scion",
            "vcs": "https://github.com/nsgomez/scion",
            "download_location": "git+https://github.com/nsgomez/scion.git",
        },
    ]

    cyclonedx = {
        "bomFormat": "CycloneDX",
        "specVersion": "1.6",
        "serialNumber": f"urn:uuid:{serial}",
        "version": 1,
        "metadata": {
            "timestamp": timestamp,
            "tools": {"components": [{"type": "application", "name": "generate_sbom.py"}]},
            "component": cyclonedx_component(main_package),
        },
        "components": [cyclonedx_component(package) for package in dependencies],
        "dependencies": [
            {"ref": main_package["purl"], "dependsOn": [package["purl"] for package in dependencies]},
            *({"ref": package["purl"], "dependsOn": []} for package in dependencies),
        ],
    }

    spdx = {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"SCD3D11-{version}",
        "documentNamespace": f"https://github.com/{repository}/sbom/SCD3D11-{version}-{serial}",
        "creationInfo": {"created": timestamp, "creators": ["Tool: generate_sbom.py"]},
        "packages": [spdx_package(package) for package in [main_package, *dependencies]],
        "relationships": [
            {"spdxElementId": "SPDXRef-DOCUMENT", "relationshipType": "DESCRIBES",
             "relatedSpdxElement": main_package["spdx_id"]},
            *({"spdxElementId": main_package["spdx_id"], "relationshipType": "DEPENDS_ON",
               "relatedSpdxElement": package["spdx_id"]} for package in dependencies),
        ],
    }

    output_directory.mkdir(parents=True, exist_ok=True)
    (output_directory / "SCD3D11.cdx.json").write_text(json.dumps(cyclonedx, indent=2) + "\n", encoding="utf-8")
    (output_directory / "SCD3D11.spdx.json").write_text(json.dumps(spdx, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
