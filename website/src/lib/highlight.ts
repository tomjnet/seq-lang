// Build-time highlighting for Seq source and for seqc console output.
// Both return HTML; every piece of text passes through escape().

export function escape(text: string): string {
  return text.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

const SEQ_TOKEN = /("(?:\\.|[^"\\])*"?)|(#.*$)|\b(model|backend|name|step|ask)\b|([A-Za-z_][A-Za-z0-9_]*)(?=\()/g;

// One line of Seq. Strings come first in the pattern, so a `#` inside a
// string is not taken for a comment.
export function seqLine(line: string): string {
  let html = '';
  let last = 0;
  for (const match of line.matchAll(SEQ_TOKEN)) {
    const [text, string, comment, keyword, call] = match;
    html += escape(line.slice(last, match.index));
    const kind = string ? 's' : comment ? 'c' : keyword ? 'k' : call ? 'f' : '';
    html += `<span class="${kind}">${escape(text)}</span>`;
    last = match.index + text.length;
  }
  return html + escape(line.slice(last));
}

export function seqLines(source: string): string[] {
  return source.replace(/\n$/, '').split('\n').map(seqLine);
}

// One line of console output: the prompt, the "[stage]" tag, diagnostics and
// the program-output rulers each get their own colour.
export function consoleLine(line: string): string {
  if (line.startsWith('$ ')) {
    return `<span class="p">$</span> <span class="cmd">${escape(line.slice(2))}</span>`;
  }
  const stage = line.match(/^(\[[a-z]+\])(\s+)(.*)$/);
  if (stage) {
    const [, tag, gap, rest] = stage;
    const body = escape(rest).replace(/\b(ok|accepted|passed)\b/g, '<span class="ok">$1</span>');
    return `<span class="t">${tag}</span>${gap}${body}`;
  }
  const error = line.match(/^(.*?: )(error\[E\d+\])(: .*)$/);
  if (error) {
    return `${escape(error[1])}<span class="e">${error[2]}</span>${escape(error[3])}`;
  }
  if (/^\s*\^\s*$/.test(line)) return `<span class="e">${escape(line)}</span>`;
  if (/^-{4} .* -{4}$/.test(line) || /^\s+hint: /.test(line) || line.startsWith('exit status')) {
    return `<span class="c">${escape(line)}</span>`;
  }
  if (line.startsWith('seqc: ')) return `<span class="e">seqc:</span>${escape(line.slice(5))}`;
  return escape(line).replace(/ {2}final$/, '  <span class="ok">final</span>');
}
