// Plain names for policy actions, shared by Flock settings and approval cards.
export const actionLabels = {
  'bot.view': 'View Bot conversations',
  'bot.talk': 'Talk to Bots',
  'bot.decide': 'Respond to Bot requests',
  'bot.settings': 'Change Bot settings',
  'bot.create': 'Create Bots',
  'bot.rename': 'Rename Bots',
  'rules.manage': 'Manage permission rules',
  'repository.read': 'Read repositories',
  'repository.write': 'Write to repositories',
  'repository.pr': 'Open or update pull requests',
  'repository.review': 'Review pull requests',
  'repository.issues': 'Manage issues',
  'secret.use': 'Use secrets',
  'session.read': 'Read T3 Sessions',
  'mcp.discover': 'List MCP tools',
  'mcp.invoke': 'Call MCP tools',
  'mcp.read_resource': 'Read MCP resources',
  'environment.inspect': 'View T3 environments',
  'environment.configure': 'Configure T3 environments',
  'environment.launch': 'Start T3 Sessions',
  'shared_memory.read': 'Read shared memory',
  'shared_memory.write': 'Write to shared memory',
  'workspace.read': 'Read workspace files',
  'workspace.write': 'Write workspace files',
  'workspace.exec': 'Run workspace commands',
  'web.search': 'Search the web',
  'web.fetch': 'Open web pages',
  'skill.list': 'List skills',
  'skill.read': 'Read skills',
  'attachment.inspect': 'View attachments',
  'channel.use': 'Use channels',
  'channel.read': 'Read channel messages',
  'channel.post': 'Post channel messages',
  't3.coordinate': 'Coordinate T3 work',
  't3.send': 'Send messages to T3',
  'automation.change': 'Change automations',
  'email.send': 'Send email',
  'data.destructive_write': 'Make destructive changes',
};
export const actionLabel = (action) => actionLabels[action] ?? action;

// Approval previews arrive as the tool's JSON arguments; show the human-readable part.
export function approvalSummary(preview) {
  if (typeof preview !== 'string' || !preview.trim()) return '';
  let value;
  try { value = JSON.parse(preview); } catch { return preview.trim(); }
  if (typeof value === 'string') return value.trim();
  if (!value || typeof value !== 'object') return preview.trim();
  const text = ['instruction', 'text', 'body', 'message', 'title', 'request', 'query', 'url']
    .map((key) => value[key]).find((item) => typeof item === 'string' && item.trim());
  if (!text) return '';
  return text.replace(/\n*\[Flock action ID:[^\]]*\]\s*/g, '').trim();
}
