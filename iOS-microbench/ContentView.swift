//
//  ContentView.swift
//  iOS-microbench
//
//  Created by Junjie on 12/09/2023.
//

import SwiftUI

func getDocumentDirectory() -> String {
    let paths = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)
    return paths[0].path
}


struct ContentView: View {
    var body: some View {
        VStack {   
            Button(action: {
                let path = getDocumentDirectory()
                test_everything(path);
            }, label: {Text("Put Shits Into CPU")});
        }
        .padding()
        VStack {
            Button(action: {
                let path = getDocumentDirectory()
                test_entry_manual(path);
            }, label: {Text("Put Just One Shit")});
        }
        .padding()
        VStack {
            Button(action: {
                test_entry_zero();
                
            }, label: {Text("Test IPC")});
        }
        .padding()
    }
}

struct ContentView_Previews: PreviewProvider {
    static var previews: some View {
        ContentView()
    }
}
