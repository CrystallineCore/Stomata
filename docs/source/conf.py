# Configuration file for the Sphinx documentation builder.
# https://www.sphinx-doc.org/en/master/usage/configuration.html

project = "stomata"
author = "Sivaprasad"
copyright = "2026, Sivaprasad"
release = "0.1.0"
version = "0.1"

extensions = [
    "myst_parser",
]

source_suffix = {".md": "markdown"}
root_doc = "index"

myst_enable_extensions = [
    "colon_fence",
]
myst_heading_anchors = 3

templates_path = ["_templates"]
exclude_patterns = ["_build", "Thumbs.db", ".DS_Store"]

html_theme = "sphinx_rtd_theme"
html_static_path = ["_static"]
html_extra_path = ["robots.txt"]
html_title = f"stomata {release}"
html_show_sourcelink = False

# Uncomment after adding the files to _static/
# html_logo = "_static/logo.png"
# html_favicon = "_static/favicon.ico"
