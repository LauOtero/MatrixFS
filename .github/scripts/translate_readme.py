#!/usr/bin/env python3
"""Traduce README.md (fuente, inglés) a README.<código>.md para el resto de
idiomas (español, chino simplificado, alemán, japonés, francés, portugués de
Brasil, ruso, coreano e italiano).

Lee .github/languages.yaml como única fuente de verdad de los idiomas.

Proveedores de traducción, en orden de prioridad:
  1. ZHIPUAI_API_KEY -> glm-4-flash
  2. OPENAI_API_KEY  -> gpt-4o
  3. ninguno         -> backend gratuito g4f
"""

import asyncio
import os
import re
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
CONFIG = ROOT / ".github" / "languages.yaml"

# Nombre del idioma tal como se le indica al modelo en el prompt.
LANG_NAMES = {
    "en": "English",
    "es": "Spanish",
    "zh-CN": "Chinese (Simplified)",
    "de": "German",
    "ja": "Japanese",
    "fr": "French",
    "pt-BR": "Brazilian Portuguese",
    "ru": "Russian",
    "ko": "Korean",
    "it": "Italian",
}

PROMPT = (
    "Translate the following Markdown document from {source_name} to {target_name}, "
    "adhering to the following rules:\n"
    "1. Maintain the original Markdown format, symbols, and spacing.\n"
    "2. Only output the translated document, with no descriptions or code fences.\n"
    "3. Translate all prose accurately, preserving line breaks, tables and lists.\n"
    "4. Do not translate the content of code blocks or inline code.\n"
    "5. Do not translate URLs, relative links, file paths, badges or HTML tags.\n"
    "6. Update the internal anchor links so they match the translated headings.\n"
    "--------------------------------\n"
    "{content}"
    "--------------------------------\n"
    "Output only the translated Markdown:\n"
)

# Envoltorio ```markdown ... ``` que algunos modelos añaden pese a lo pedido.
FENCE_RE = re.compile(r"^\s*```(?:markdown|md)?\s*\n(.*)\n```\s*$", re.DOTALL)
# Preámbulos conversacionales típicos ("Sure! Here is the translation:").
PREAMBLE_RE = re.compile(
    r"^(?:sure[,!]?\s*|of course[,!]?\s*|here(?:'s| is)[^\n]*:\s*\n+)",
    re.IGNORECASE,
)


def _clean(text: str) -> str:
    """Quita envoltorios de código y preámbulos conversacionales del modelo."""
    text = text.strip()
    match = FENCE_RE.match(text)
    if match:
        text = match.group(1).strip()
    return PREAMBLE_RE.sub("", text, count=1).strip()


async def _via_zhipuai(query: str) -> str:
    from zhipuai import ZhipuAI

    client = ZhipuAI(api_key=os.environ["ZHIPUAI_API_KEY"])
    response = client.chat.asyncCompletions.create(
        model="glm-4-flash", messages=[{"role": "user", "content": query}]
    )
    task_id = response.id
    status = ""
    while status not in ("SUCCESS", "FAILED"):
        result = client.chat.asyncCompletions.retrieve_completion_result(id=task_id)
        status = result.task_status
        await asyncio.sleep(0.5)
    if status == "FAILED":
        raise RuntimeError("ZhipuAI devolvió FAILED")
    return result.choices[0].message.content


async def _via_openai(query: str) -> str:
    from openai import AsyncOpenAI

    client = AsyncOpenAI(api_key=os.environ["OPENAI_API_KEY"])
    response = await client.chat.completions.create(
        model="gpt-4o", messages=[{"role": "user", "content": query}]
    )
    return response.choices[0].message.content


async def _via_g4f(query: str) -> str:
    import g4f

    return await g4f.ChatCompletion.create_async(
        model="gpt-4o", messages=[{"role": "user", "content": query}]
    )


async def translate(content: str, source_name: str, target_name: str) -> str:
    query = PROMPT.format(
        source_name=source_name, target_name=target_name, content=content
    )
    if os.environ.get("ZHIPUAI_API_KEY"):
        return await _via_zhipuai(query)
    if os.environ.get("OPENAI_API_KEY"):
        return await _via_openai(query)
    return await _via_g4f(query)


def main() -> int:
    cfg = yaml.safe_load(CONFIG.read_text(encoding="utf-8"))
    source_file = ROOT / cfg["source_file"]
    source_lang = cfg["source_language"]
    source_name = LANG_NAMES.get(source_lang, source_lang)

    content = source_file.read_text(encoding="utf-8")
    targets = [lang for lang in cfg["languages"] if lang != source_lang]

    async def run() -> None:
        async def one(lang: str) -> None:
            target_name = LANG_NAMES.get(lang, lang)
            text = _clean(await translate(content, source_name, target_name))
            if not text:
                raise RuntimeError(f"Traducción vacía para '{lang}'")
            if text.count("```") % 2:
                raise RuntimeError(
                    f"Markdown con cercas de código desbalanceadas en '{lang}'"
                )
            out = ROOT / f"README.{lang}.md"
            out.write_text(text + "\n", encoding="utf-8")
            print(f"OK -> {out.name} ({len(text)} caracteres)")

        await asyncio.gather(*(one(lang) for lang in targets))

    asyncio.run(run())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
