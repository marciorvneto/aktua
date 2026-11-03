if vim.b.current_syntax then
  return
end

-- Define syntax groups
vim.cmd([[
  syntax case match

  syntax keyword tinyctrlKeyword let fn return
  syntax keyword tinyctrlConditional if elif else
  syntax keyword tinyctrlRepeat for in

  syntax match tinyctrlDelimiter /[{}()\[\],]/
  syntax match tinyctrlOperator  /[-+*\/^@<>=]/

  syntax match tinyctrlNumber /\v<\d+(\.\d+)?([eE][+-]?\d+)?>/

  syntax match tinyctrlFunction /\v<[A-Za-z_][A-Za-z0-9_]*\ze\s*\(/

  syntax region tinyctrlString start=/"/ skip=/\\./ end=/"/

  syntax match tinyctrlComment /\/\/.*/
]])

-- Associate groups with colorscheme highlights
local groups = {
  tinyctrlKeyword     = "Keyword",
  tinyctrlConditional = "Conditional",
  tinyctrlRepeat      = "Repeat",
  tinyctrlDelimiter   = "Delimiter",
  tinyctrlOperator    = "Operator",
  tinyctrlNumber      = "Number",
  tinyctrlFunction    = "Function",
  tinyctrlString      = "String",
  tinyctrlComment     = "Comment",
}

for group, link in pairs(groups) do
  vim.api.nvim_set_hl(0, group, {
    link = link,
    default = true,
  })
end

vim.b.current_syntax = "tinyctrl"

