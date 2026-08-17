# docs/

Generated images. Both come out of the project-video pipeline, so neither is
hand-made and both can be regenerated:

    cd ~/projects/infrastructure/stoatworks-backend/video/projects/vectrix
    python3 render.py && python3 build.py
    cp out/vectrix-thumb.png   ~/projects/resolume/vectrix/docs/video-thumb.png
    cp work/thumb-source.png   ~/projects/resolume/vectrix/docs/hero.png

- `video-thumb.png` — the YouTube thumbnail. YouTube fetches it from this
  repository's raw URL at **upload time only**, so it has to be committed and
  pushed before the video goes up; there is no API to change it afterwards.
- `instagram-cover.jpg` — the Reel's cover. Instagram fetches it from this
  repository's raw URL, and the URL is registered SHA-pinned in
  `stoatworks-backend/video/social-covers.json`; pinning to `main` would let a
  later commit change the cover of a post that is already up.
- `hero.png` — the still the website uses. `stoatworks-website`'s
  `scripts/shots.json` points at this path, and `make_screens.py` deletes any
  hero it did not produce itself, which is why it lives here rather than being
  committed into the site.
