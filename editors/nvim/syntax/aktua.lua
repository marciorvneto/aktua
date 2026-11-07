if vim.b.current_syntax then
  return
end

-- Define syntax groups
vim.cmd([[
  syntax case match

  syntax keyword aktuaKeyword let fn return
  syntax keyword aktuaConditional if elif else
  syntax keyword aktuaRepeat for in

  syntax match aktuaDelimiter /[{}()\[\],]/
  syntax match aktuaOperator  /[-+*\/^@<>=]/

  syntax match aktuaNumber /\v<\d+(\.\d+)?([eE][+-]?\d+)?>/

  syntax match aktuaFunction /\v<[A-Za-z_][A-Za-z0-9_]*\ze\s*\(/

  " Native functions implemented in C (native_fn_names in main.c)
  syntax keyword aktuaBuiltin assert ls print import complex abs real imag arg log exp sin cos tan asin acos atan sinh cosh tanh asinh acosh atanh floor ceil round sqr sqrt max min sum prod range linspace len push mean plot poly polymod polydiv polygcd poles impulse step

  syntax region aktuaString start=/"/ skip=/\\./ end=/"/

  syntax match aktuaComment /\/\/.*/
]])

-- Associate groups with colorscheme highlights
local groups = {
  aktuaKeyword     = "Keyword",
  aktuaConditional = "Conditional",
  aktuaRepeat      = "Repeat",
  aktuaDelimiter   = "Delimiter",
  aktuaOperator    = "Operator",
  aktuaNumber      = "Number",
  aktuaFunction    = "Function",
  aktuaBuiltin     = "@function.builtin",
  aktuaString      = "String",
  aktuaComment     = "Comment",
}

for group, link in pairs(groups) do
  vim.api.nvim_set_hl(0, group, {
    link = link,
    default = true,
  })
end

vim.b.current_syntax = "aktua"

