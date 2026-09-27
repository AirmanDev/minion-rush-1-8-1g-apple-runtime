import AppKit
import SwiftUI

struct InstallerCard<Content: View>: View {
  let title: String
  var complete = false
  @ViewBuilder let content: Content

  var body: some View {
    VStack(alignment: .leading, spacing: 14) {
      HStack {
        Text(title).font(.headline)
        Spacer()
        if complete {
          Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
            .accessibilityLabel("Complete")
        }
      }
      content
    }
    .padding(20)
    .frame(maxWidth: .infinity, alignment: .leading)
    .background(.background, in: RoundedRectangle(cornerRadius: 16))
    .overlay { RoundedRectangle(cornerRadius: 16).strokeBorder(.primary.opacity(0.08)) }
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
      Text(title).font(.subheadline.weight(.medium))
      InstallerPopUp(
        options: options, selection: $selection, title: title,
        identifier: identifier, enabled: enabled
      ).frame(maxWidth: .infinity, minHeight: 44, maxHeight: 44)
        .background(.quaternary, in: RoundedRectangle(cornerRadius: 8))
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
    button.isBordered = false
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
