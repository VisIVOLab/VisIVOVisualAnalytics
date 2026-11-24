"""FITS files under a name that does not say so (issue #501): the ATCA images
``AGN_SHORES_I`` / ``_Q`` / ``_U`` have no extension. They are FITS by their
first card, in the browser's listing, the open route and the SODA media type;
a file that merely has no extension stays what its content says."""
from __future__ import annotations

import gzip
import os

import numpy as np
import pytest
from astropy.io import fits
from httpx import AsyncClient

from app import dependencies as deps
from app.routers.soda import _media_type


def _image(path, name="AGN_SHORES_I"):
    target = path / name
    fits.PrimaryHDU(np.random.default_rng(0).normal(size=(32, 32)).astype("f4")).writeto(target)
    return target


def test_a_fits_is_known_by_its_first_card_not_its_name(tmp_path):
    plain = _image(tmp_path)
    assert deps._is_fits(plain)
    assert deps._is_supported_dataset(plain)
    assert _media_type(plain) == "application/fits"


def test_a_gzipped_fits_without_a_fits_name_is_fits(tmp_path):
    plain = _image(tmp_path, "img")
    zipped = tmp_path / "img_copy"
    zipped.write_bytes(gzip.compress(plain.read_bytes()))
    assert deps._is_fits(zipped)


def test_a_small_gzipped_fits_without_a_name_is_fits(tmp_path):
    plain = tmp_path / "plain"
    fits.PrimaryHDU(np.zeros((8, 8), "f4")).writeto(plain)
    zipped = tmp_path / "tiny"
    zipped.write_bytes(gzip.compress(plain.read_bytes()))
    assert zipped.stat().st_size < 2880
    assert deps._is_fits(zipped)


def test_a_file_unreadable_at_first_is_read_again(tmp_path):
    path = _image(tmp_path)
    path.chmod(0)
    try:
        if os.access(path, os.R_OK):
            pytest.skip("permissions are not enforced here (root)")
        assert not deps._is_fits(path)
    finally:
        path.chmod(0o644)
    assert deps._is_fits(path)   # the failure was not remembered


def test_a_symlink_to_an_extensionless_fits_is_fits(tmp_path):
    plain = _image(tmp_path)
    link = tmp_path / "link"
    os.symlink(plain, link)
    assert deps._is_fits(link)
    assert deps._is_supported_dataset(link)


def test_other_extensionless_files_are_not_fits(tmp_path):
    text = tmp_path / "README"
    text.write_text("SIMPLE  = this is prose\n" * 400)
    binary = tmp_path / "blob"
    binary.write_bytes(os.urandom(8192))
    false_card = tmp_path / "falsefits"   # SIMPLE = F is not a conforming file
    false_card.write_bytes(b"SIMPLE  =                    F".ljust(80) + b" " * 4000)
    short = tmp_path / "short"
    short.write_bytes(b"SIMPLE  =                    T".ljust(80))
    notfits_gz = tmp_path / "table"
    notfits_gz.write_bytes(gzip.compress(b"a,b\n" * 2000))
    for path in (text, binary, false_card, short, notfits_gz):
        assert not deps._is_fits(path), path.name
        assert not deps._is_supported_dataset(path), path.name
    assert _media_type(binary) == "application/octet-stream"


def test_a_known_other_suffix_is_not_read(tmp_path):
    imposter = tmp_path / "notes.csv"
    fits.PrimaryHDU(np.zeros((8, 8), "f4")).writeto(imposter)
    assert not deps._is_fits(imposter)


def test_a_rewritten_file_is_sniffed_again(tmp_path):
    path = tmp_path / "x"
    path.write_bytes(b"a" * 4000)
    assert not deps._is_fits(path)
    path.unlink()
    _image(tmp_path, "x")
    assert deps._is_fits(path)


@pytest.mark.asyncio
async def test_the_listing_flags_extensionless_fits(client: AsyncClient, auth_headers, tmp_path):
    for name in ("AGN_SHORES_I", "AGN_SHORES_Q"):
        _image(tmp_path, name)
    (tmp_path / "README").write_text("hello " * 1000)
    resp = await client.get("/v1/files/list", params={"path": str(tmp_path)}, headers=auth_headers)
    flags = {e["name"]: e["is_fits"] for e in resp.json()["entries"]}
    assert flags == {"AGN_SHORES_I": True, "AGN_SHORES_Q": True, "README": False}


@pytest.mark.asyncio
async def test_an_extensionless_fits_opens_and_its_header_previews(
        client: AsyncClient, auth_headers, tmp_path):
    path = _image(tmp_path)
    resp = await client.post("/v1/datasets/open", json={"path": str(path)}, headers=auth_headers)
    assert resp.json()["valid"], resp.json()
    header = await client.post("/v1/files/header", json={"path": str(path)}, headers=auth_headers)
    assert header.json()["valid"]
    verdict = await client.post("/v1/files/classify", json={"path": str(path)}, headers=auth_headers)
    assert verdict.json()["opener"] == "dataset"


@pytest.mark.asyncio
async def test_a_symlink_to_an_extensionless_fits_opens(client: AsyncClient, auth_headers, tmp_path):
    target = _image(tmp_path)
    link = tmp_path / "linked"
    os.symlink(target, link)
    resp = await client.post("/v1/datasets/open", json={"path": str(link)}, headers=auth_headers)
    assert resp.json()["valid"], resp.json()


@pytest.mark.asyncio
async def test_an_extensionless_file_that_is_not_fits_stays_unsupported(
        client: AsyncClient, auth_headers, tmp_path):
    path = tmp_path / "AGN_SHORES_I"
    path.write_bytes(os.urandom(8192))
    resp = await client.post("/v1/datasets/open", json={"path": str(path)}, headers=auth_headers)
    body = resp.json()
    assert not body["valid"] and body["error"].startswith("Unsupported file type")
