#!/usr/bin/env python3
"""Genera README.en.md a partir de README.md (español).

La acción Lin-jun-xiang/action-translate-readme omite el idioma inglés cuando el
README raíz no está en inglés, por lo que la versión inglesa se genera aquí con
el mismo proveedor de IA que la acción (misma prioridad y mismas claves).

Proveedores, en orden de prioridad:
  1. ZHIPUAI_API_KEY -> glm-4-flash
  2. OPENAI_API_KEY  -> gpt-4o
  3. ninguno         -> backend gratuito g4f

Las dependencias (openai, zhipuai, g4f) ya las instala la acción en el mismo job.
"""

import asyncio
import os
import sys
from pathlib import Path

SOURCE = Path("README.md")
TARGET = Path("README.en.md")

PROMPT = (
    "Translate the following Markdown document from Spanish to [English], "
    "adhering to the following rules:\n"
    "1. Maintain the original Markdown format, symbols, and spacing.\n"
    "2. Only output the translated document, with no descriptions or code fences.\n"
    "3. Translate all prose accurately, preserving line breaks and tables.\n"
    "4. Do not translate the content of code blocks or inline code.\n"
    "5. Do not translate URLs, relative links, file paths, badges or HTML tags.\n"
    "--------------------------------\n"
    "{content}"
    "--------------------------------\n"
    "Output only the translated Markdown:\n"
)


async def translate(content: str) -> str:
    query = PROMPT.format(content=content)
    zhipuai_key = os.environ.get("ZHIPUAI_API_KEY", "")
    openai_key = os.environ.get("OPENAI_API_KEY", "")

    if zhipuai_key:
        from zhipuai import ZhipuAI

        client = ZhipuAI(api_key=zhipuai_key)
        response = client.chat.asyncCompletions.create(
            model="glm-4-flash", messages=[{"role": "user", "content": query}]
        )
        task_id = response.id
        status = ""
        while status not in ("SUCCESS", "FAILED"):
            result = client.chat.asyncCompletions.retrieve_completion_result(id=task_id)
            status = result.task_status
            await asyncio.sleep(0.5)
        return result.choices[0].message.content

    if openai_key:
        from openai import AsyncOpenAI

        client = AsyncOpenAI(api_key=openai_key)
        response = await client.chat.completions.create(
            model="gpt-4o", messages=[{"role": "user", "content": query}]
        )
        return response.choices[0].message.content

    import g4f

    return await g4f.ChatCompletion.create_async(
        model="gpt-4o", messages=[{"role": "user", "content": query}]
    )


def main() -> int:
    content = SOURCE.read_text(encoding="utf-8")
    result = asyncio.run(translate(content))
    if not result or not result.strip():
        print("ERROR: la traducción al inglés quedó vacía.", file=sys.stderr)
        return 1
    TARGET.write_text(result.strip() + "\n", encoding="utf-8")
    print(f"Escrito {TARGET} ({len(result)} caracteres).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
