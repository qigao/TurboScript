@echo off
REM Regenerate lexer and parser from source files

echo Regenerating lexer...
re2c exprtk_lexer.re -o exprtk_lexer.c
if %ERRORLEVEL% NEQ 0 (
    echo Error: re2c failed
    exit /b 1
)

echo Regenerating parser...
lemon exprtk_grammar.y
if %ERRORLEVEL% NEQ 0 (
    echo Error: lemon failed
    exit /b 1
)

echo Done! Remember to rebuild the project.
