import AppKit
import SwiftUI

struct BuilderPanel<Content: View>: View {
  @ViewBuilder let content: Content

  var body: some View {
    content
      .padding(20)
      .frame(maxWidth: .infinity, alignment: .leading)
      .background(BuilderPalette.card, in: RoundedRectangle(cornerRadius: 16))
      .overlay {
        RoundedRectangle(cornerRadius: 16).strokeBorder(BuilderPalette.border)
      }
  }
}

struct InstallerCard<Content: View>: View {
  let title: String
  var complete = false
  @ViewBuilder let content: Content

  var body: some View {
    BuilderPanel {
      VStack(alignment: .leading, spacing: 14) {
        HStack {
          Text(title).font(.headline).foregroundStyle(BuilderPalette.text)
          Spacer()
          if complete {
            Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
              .accessibilityLabel("Complete")
          }
        }
        content
      }
    }
  }
}

struct InstallerOption {
  let id: String
  let label: String
}

struct InstallerSelection: View {
  let title: String
  let options: [InstallerOption]
  @Binding var selection: String
  let identifier: String
  @Environment(\.isEnabled) private var enabled

  var body: some View {
    VStack(alignment: .leading, spacing: 6) {
      Text(title)
        .font(.subheadline.weight(.medium))
        .foregroundStyle(BuilderPalette.text)
      InstallerPopUp(
        options: options, selection: $selection, title: title,
        identifier: identifier, enabled: enabled
      ).frame(maxWidth: .infinity, minHeight: 44, maxHeight: 44)
        .background(BuilderPalette.field, in: RoundedRectangle(cornerRadius: 8))
    }.frame(maxWidth: .infinity, alignment: .leading)
  }
}

private struct InstallerPopUp: NSViewRepresentable {
  let options: [InstallerOption]
  @Binding var selection: String
  let title: String
  let identifier: String
  let enabled: Bool

  func makeNSView(context: Context) -> NSPopUpButton {
    let button = NSPopUpButton(frame: .zero, pullsDown: false)
    button.controlSize = .large
    button.bezelStyle = .rounded
    button.appearance = NSAppearance(named: .aqua)
    button.isBordered = true
    button.target = context.coordinator
    button.action = #selector(Coordinator.select(_:))
    button.setAccessibilityLabel(title)
    button.setAccessibilityIdentifier(identifier)
    button.setContentHuggingPriority(.defaultLow, for: .horizontal)
    return button
  }

  func updateNSView(_ button: NSPopUpButton, context: Context) {
    context.coordinator.selection = $selection
    if button.itemTitles != options.map(\.label)
      || button.itemArray.map({ $0.representedObject as? String })
        != options.map({ Optional($0.id) })
    {
      button.removeAllItems()
      for option in options {
        button.addItem(withTitle: option.label)
        button.lastItem?.representedObject = option.id
      }
    }
    button.selectItem(at: options.firstIndex(where: { $0.id == selection }) ?? 0)
    button.isEnabled = enabled
  }

  func makeCoordinator() -> Coordinator { Coordinator(selection: $selection) }

  func sizeThatFits(_ proposal: ProposedViewSize, nsView: NSPopUpButton, context: Context)
    -> CGSize?
  {
    CGSize(width: proposal.width ?? nsView.intrinsicContentSize.width, height: 44)
  }

  @MainActor
  final class Coordinator: NSObject {
    var selection: Binding<String>
    init(selection: Binding<String>) { self.selection = selection }
    @objc func select(_ sender: NSPopUpButton) {
      if let value = sender.selectedItem?.representedObject as? String {
        selection.wrappedValue = value
      }
    }
  }
}

struct InstallerField: View {
  let title: String
  @Binding var value: String
  var prompt = ""
  var problem: String?

  var body: some View {
    VStack(alignment: .leading, spacing: 6) {
      Text(title).font(.subheadline.weight(.medium))
      TextField(title, text: $value, prompt: Text(prompt))
        .labelsHidden().textFieldStyle(.roundedBorder).controlSize(.large)
        .accessibilityLabel(title)
      if let problem {
        Label(problem, systemImage: "exclamationmark.circle")
          .font(.caption).foregroundStyle(.red)
      }
    }
  }
}

struct BuilderButtonStyle: ButtonStyle {
  enum Role {
    case primary
    case secondary
    case language(Bool)
  }
  let role: Role
  @Environment(\.isEnabled) private var enabled

  func makeBody(configuration: Configuration) -> some View {
    let selected: Bool
    let primary: Bool
    switch role {
    case .primary:
      selected = true
      primary = true
    case .secondary:
      selected = false
      primary = false
    case .language(let active):
      selected = active
      primary = active
    }
    return configuration.label
      .font(.system(size: 14, weight: .semibold))
      .foregroundStyle(primary ? BuilderPalette.navy : BuilderPalette.text)
      .frame(minHeight: 40)
      .padding(.horizontal, 15)
      .background(
        enabled
          ? (primary
            ? BuilderPalette.gold.opacity(configuration.isPressed ? 0.83 : 1)
            : BuilderPalette.card)
          : BuilderPalette.field,
        in: RoundedRectangle(cornerRadius: 9)
      )
      .overlay {
        RoundedRectangle(cornerRadius: 9)
          .strokeBorder(selected ? Color.clear : BuilderPalette.border, lineWidth: 1)
      }
      .opacity(enabled ? 1 : 0.62)
      .contentShape(RoundedRectangle(cornerRadius: 9))
  }
}

struct BuilderInputRow: View {
  let title: String
  var hint: String?
  let value: String
  let button: String
  var identifier: String
  let choose: () -> Void

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      HStack {
        Text(title).font(.system(size: 15, weight: .semibold))
        Spacer()
        if let hint {
          Text(hint).font(.caption).foregroundStyle(BuilderPalette.muted)
        }
      }.foregroundStyle(BuilderPalette.text)
      HStack(spacing: 12) {
        Text(value)
          .foregroundStyle(BuilderPalette.muted)
          .font(.system(size: 13))
          .lineLimit(1)
          .truncationMode(.middle)
          .frame(maxWidth: .infinity, minHeight: 44, alignment: .leading)
          .padding(.horizontal, 12)
          .background(BuilderPalette.field, in: RoundedRectangle(cornerRadius: 9))
          .overlay {
            RoundedRectangle(cornerRadius: 9)
              .strokeBorder(BuilderPalette.border, lineWidth: 1)
          }
        Button(button, action: choose)
          .buttonStyle(BuilderButtonStyle(role: .secondary))
          .frame(minWidth: 156)
          .accessibilityIdentifier(identifier)
      }
    }
  }
}
